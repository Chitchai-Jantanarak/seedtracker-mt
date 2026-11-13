/*
 * scan.cl - GPU prefilter for the seed scan. One work-item per seed.
 *
 * For its seed the kernel fills the draw window, then walks the whole event
 * chain exactly like ChainMatcher (compute/chain.hpp): every offset, every gap
 * of every later event, every event simulated with sf_step_event from
 * sim_core.h. If ANY path matches every event's post-rule, the seed is appended
 * to the hit buffer. The host then re-runs the CPU matcher on exactly those
 * seeds, so stats, CSV rows, volatility and examples come from the CPU code.
 *
 * The kernel also counts invalid events (reroll or draw cap, window overrun)
 * the way ChainMatcher does. It never stops at the first match, so the count
 * covers the same tree the CPU walks.
 *
 * Short window: the CPU sizes its draw window for the worst case (a dist-* pick
 * may read 64 words, a reroll event 64 picks), which is far more than events
 * use in practice. The kernel fills only SF_K_W draws, a "typical" window. A
 * valid event never reads past its window, so valid events are exact. If any
 * event comes back invalid while SF_K_W is smaller than the CPU window
 * (SF_K_EXACT == 0), the kernel cannot tell a real cap from a window cut-off, so
 * it reports the seed as "uncertain": the host re-runs it on the CPU and takes
 * that run's invalid count instead of this kernel's. Hit seeds go to `hits`,
 * uncertain seeds to `uncertain`; a seed is in at most one of them.
 *
 * Build: the host (opencl_scanner.cpp) concatenates
 *   1. a prelude that #defines the SF_K_* constants and array initialisers below,
 *   2. sim_core.h (replacing the #include line below), with custom_rule.h spliced
 *      into sim_core.h in place of its own #include line,
 *   3. this file.
 *
 * Prelude symbols:
 *   SF_K_W                   draws the kernel fills per seed (compile-time private array size)
 *   SF_K_EXACT               1 when SF_K_W equals the CPU window, 0 for the short window
 *   SF_K_EVENTS              number of logged events (>= 1)
 *   SF_K_MAPPING             SF_MAP_* used to fill the window
 *   SF_K_LAZY_K              raw-word budget of sf_seed_fill_lazy (host guarantees > 0)
 *   SF_K_MODEL, SF_K_PICK, SF_K_RNG_STEP, SF_K_VALUE_STEP, SF_K_DOMAIN_COUNT
 *                            the sf_params fields
 *   SF_K_OFFSET_BEGIN/END/STEP
 *   SF_K_GAP_MIN_INIT, SF_K_GAP_MAX_INIT            per-event gap range initialisers
 *   SF_K_PREFIX_LEN_INIT, SF_K_PREFIX_INIT          per-event post-rule prefix
 *   SF_K_INCLUDE_INIT, SF_K_EXCLUDE_INIT            per-event include and exclude masks
 */
#include "seedtracker/core/sim_core.h"

__constant uint sf_k_gap_min[SF_K_EVENTS] = SF_K_GAP_MIN_INIT;
__constant uint sf_k_gap_max[SF_K_EVENTS] = SF_K_GAP_MAX_INIT;
__constant int sf_k_prefix_len[SF_K_EVENTS] = SF_K_PREFIX_LEN_INIT;
__constant int sf_k_prefix[SF_K_EVENTS][5] = SF_K_PREFIX_INIT;
__constant uint sf_k_include[SF_K_EVENTS] = SF_K_INCLUDE_INIT;
__constant uint sf_k_exclude[SF_K_EVENTS] = SF_K_EXCLUDE_INIT;

/* Same predicate as rule_matches() in post/rules.hpp. */
static inline int sf_k_match(int k, __private const sf_event* ev)
{
    int i;
    uint seen;
    for (i = 0; i < sf_k_prefix_len[k]; ++i) {
        if (ev->elim[i] != sf_k_prefix[k][i]) return 0;
    }
    seen = 0u;
    for (i = 0; i < 5; ++i) seen |= 1u << ev->elim[i];
    if ((seen & sf_k_include[k]) != sf_k_include[k]) return 0;
    if (seen & sf_k_exclude[k]) return 0;
    return 1;
}

#if SF_K_LAZY_K == 0
#error "scan.cl needs the lazy seed fill: the host must keep the window within its limits"
#endif

/*
 * counters[0] = number of hit seeds, counters[2] = number of uncertain seeds
 *               (either may exceed hit_cap; only the first hit_cap of each are
 *               stored and the host then rescans a smaller batch),
 * counters[1] = invalid events of the seeds that are not uncertain, summed.
 */
__kernel void sf_scan(uint seed_base, uint count, __global uint* hits, __global uint* uncertain_seeds,
                      uint hit_cap, __global uint* counters)
{
    size_t gid = get_global_id(0);
    uint32_t seed;
    uint32_t draws[SF_K_W];
    sf_params p;
    sf_event ev;
    uint32_t invalid = 0u;
    int hit = 0;
    int uncertain = 0;
    uint32_t off;
    uint32_t end_of[SF_K_EVENTS];
    uint32_t gap_of[SF_K_EVENTS];

    if (gid >= (size_t)count) return;
    seed = seed_base + (uint32_t)gid;

    p.model = SF_K_MODEL;
    p.pick = SF_K_PICK;
    p.rng_step = SF_K_RNG_STEP;
    p.value_step = SF_K_VALUE_STEP;
    p.domain_count = SF_K_DOMAIN_COUNT;

    /*
     * Build the draws from the first few init words without the 624-word state
     * (sf_seed_fill_lazy). reject31 may run out of its word budget; then the
     * full state is used.
     */
    {
        uint32_t scratch[SF_K_LAZY_K + 1];
        if (!sf_seed_fill_lazy(seed, SF_K_MAPPING, draws, SF_K_W, SF_K_LAZY_K, scratch)) {
            sf_mt_state st;
            sf_mt_init(&st, seed);
            sf_fill_draws(&st, SF_K_MAPPING, draws, SF_K_W);
        }
    }

    for (off = SF_K_OFFSET_BEGIN; off < SF_K_OFFSET_END; off += SF_K_OFFSET_STEP) {
        /*
         * Iterative form of ChainMatcher::descend. Level k simulates event k at
         * `start`. A match at k < n-2 descends to k+1 with that level's first
         * gap; a match at k == n-2 tries every gap of the last event inline
         * (ChainMatcher::last_event). Backtracking advances the deepest level
         * that still has a gap left. gap_of[]/end_of[] are the explicit stack.
         */
        int k = 0;
        uint32_t start = off;
        for (;;) {
            int deeper = 0;
            sf_step_event(draws, SF_K_W, start, &p, &ev);
            if (!ev.valid) {
                ++invalid;
                if (!SF_K_EXACT) uncertain = 1;
            } else if (sf_k_match(k, &ev)) {
                if (k == SF_K_EVENTS - 1) {
                    hit = 1; /* single-event chain */
                } else if (k == SF_K_EVENTS - 2) {
                    uint32_t last_end = start + ev.draws_used;
                    uint32_t g;
                    for (g = sf_k_gap_min[k + 1]; g <= sf_k_gap_max[k + 1]; ++g) {
                        sf_step_event(draws, SF_K_W, last_end + g, &p, &ev);
                        if (!ev.valid) {
                            ++invalid;
                            if (!SF_K_EXACT) uncertain = 1;
                        } else if (sf_k_match(k + 1, &ev)) {
                            hit = 1;
                        }
                    }
                } else {
                    end_of[k] = start + ev.draws_used;
                    ++k;
                    gap_of[k] = sf_k_gap_min[k];
                    start = end_of[k - 1] + gap_of[k];
                    deeper = 1;
                }
            }
            if (deeper) continue;
            while (k > 0 && gap_of[k] >= sf_k_gap_max[k]) --k;
            if (k == 0) break;
            ++gap_of[k];
            start = end_of[k - 1] + gap_of[k];
        }
    }

    if (uncertain) {
        uint idx = atomic_inc(&counters[2]);
        if (idx < hit_cap) uncertain_seeds[idx] = seed;
        return;
    }
    if (invalid) atomic_add(&counters[1], invalid);
    if (hit) {
        uint idx = atomic_inc(&counters[0]);
        if (idx < hit_cap) hits[idx] = seed;
    }
}
