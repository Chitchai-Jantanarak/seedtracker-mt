/*
 * custom_rule.h - your own pick rule and/or elimination model, for rules the
 * built-in --pick / --model values do not cover.
 *
 * Select them on the command line with `--pick custom` and/or `--model custom`.
 * Nothing else needs editing: change this file, rebuild, run. The CPU scan and the
 * OpenCL GPU kernel both compile this exact text (sim_core.h includes it, and the
 * GPU build splices the file into the kernel source), so both backends use your rule.
 *
 * What each function decides
 *   sf_custom_pick   One draw -> one index in [0, choices). Used whenever a model
 *                    asks for a pick and `--pick custom` is set. It is the "script
 *                    rand(...) then reduce to a choice" step.
 *   sf_custom_event  One whole event: which of rooms 1..6 are eliminated, in what
 *                    order, and which room survives. Used when `--model custom` is
 *                    set. It may call sf_pick(), which dispatches to sf_custom_pick
 *                    when `--pick custom` is set, so the two compose.
 *
 * The 4 rules (the GPU compiler gives no error for most of these, it just
 * produces different answers, so keep to them)
 *   1. Consume random draws only through the draw buffer. In sf_custom_pick read
 *      buf[*pos] and nothing else; in sf_custom_event call sf_pick() so the
 *      buffer position, rng_step and the end-of-window check stay correct.
 *   2. Before reading buf[*pos], check `if (*pos >= W) { *ok = 0; return 0; }`.
 *      After reading, advance `*pos += p->rng_step;` (one step per draw used).
 *   3. Cap every loop. When a cap is hit, return from sf_custom_event WITHOUT
 *      setting out->valid = 1 (the event then counts as invalid). No recursion,
 *      no heap, no double, no static or global variables, only fixed-size
 *      arrays. This is plain C that must also build as OpenCL C 1.2.
 *   4. The event must fit sf_event: out->elim[0..4] and out->survivor are rooms
 *      1..6, out->valid = 1 and out->draws_used = (slots consumed, counted from
 *      `pos`) when the event is complete. Put your tunable numbers in this file as
 *      `#define SF_CUSTOM_...` constants; the file is embedded in the GPU kernel,
 *      so a rebuild is all it takes (no run-time arguments reach it).
 *
 * How to verify a change
 *   - Run the same scan with `--backend cpu` and with `--backend opencl` and
 *     compare the sorted CSV rows (use a calibration that leaves few candidates,
 *     e.g. `--seed-end 16777216 --calib 5,1 --depth 2`).
 *   - Compare against a plain C++ reference of the target's logic: emit events
 *     with `--emit-events` for a few seeds and check them by hand or with a
 *     small program that uses std::mt19937 directly.
 *
 * Both defaults below behave exactly like the built-ins (`--pick direct` and
 * `--model remaining`), so selecting `custom` before editing changes nothing.
 */
#ifndef SEEDTRACKER_CORE_CUSTOM_RULE_H
#define SEEDTRACKER_CORE_CUSTOM_RULE_H

/*
 * Most picks one custom event makes (each pick reads one draw slot, times
 * --rng-step). The scan sizes every seed's draw window from this, so if your
 * sf_custom_event can pick more than 5 times (rerolls, bonus picks), raise it.
 * Too low: valid events run past the window and are dropped as invalid, on CPU
 * and GPU alike, with no error. Too high: only slower and fewer GPU-eligible runs.
 */
#define SF_CUSTOM_MAX_PICKS 5u

/*
 * 1 if the custom model eliminates five distinct rooms and leaves a survivor
 * (like remaining/shuffle), 0 if it is roll-like (values repeat, no survivor:
 * the CSV writes survivor 0).
 */
#define SF_CUSTOM_ELIMINATES 1

/* Example constant, used by the commented-out pick below. */
#define SF_CUSTOM_BASE 1

/*
 * One pick: reads buf[*pos], advances *pos by p->rng_step, returns an index in
 * [0, choices). sf_pick already returned for choices <= 1 before calling this.
 *
 * Default: same as `--pick direct` (raw % choices).
 */
SF_INLINE int sf_custom_pick(SF_PRIVATE const uint32_t* buf, uint32_t W, SF_PRIVATE uint32_t* pos,
                             SF_PRIVATE const sf_params* p, int choices, SF_PRIVATE int* ok)
{
    uint32_t raw;
    if (*pos >= W) {
        *ok = 0;
        return 0;
    }
    raw = buf[*pos];
    *pos += p->rng_step;

    /*
     * Example: a target that does `rand(base, base + span) % choices`. The
     * built-in fixed-mod drops the base offset (it takes the value's distance
     * from --base-min); this keeps it. Uses --base-min/--base-max/--value-step
     * through p->domain_count and p->value_step.
     *
     *   return (int)((SF_CUSTOM_BASE + (raw % p->domain_count) * p->value_step) % (uint32_t)choices);
     */
    return (int)(raw % (uint32_t)choices);
}

/*
 * One whole event, reading the draw buffer from buf[pos]. `out` is NOT
 * pre-filled here: set every field (valid, elim[0..4], survivor, draws_used).
 *
 * Default: same as `--model remaining` (erase the picked room, the rest keep
 * their order), using sf_pick for every pick.
 */
SF_INLINE void sf_custom_event(SF_PRIVATE const uint32_t* buf, uint32_t W, uint32_t pos,
                               SF_PRIVATE const sf_params* p, SF_PRIVATE sf_event* out)
{
    uint32_t cur = pos;
    int ok = 1;
    int rooms[6];
    int n = 6;
    int i, j;
    out->valid = 0;
    out->survivor = 0;
    out->draws_used = 0u;
    for (i = 0; i < 5; ++i) out->elim[i] = 0;

    for (i = 0; i < 6; ++i) rooms[i] = i + 1;
    for (i = 0; i < 5; ++i) {
        int idx = sf_pick(buf, W, &cur, p, n, &ok);
        if (!ok) return;
        out->elim[i] = rooms[idx];
        for (j = idx; j < n - 1; ++j) rooms[j] = rooms[j + 1];
        --n;
    }
    out->survivor = rooms[0];
    out->draws_used = cur - pos;
    out->valid = 1;
}

#endif /* SEEDTRACKER_CORE_CUSTOM_RULE_H */
