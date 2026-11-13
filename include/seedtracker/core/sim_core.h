/*
 * sim_core.h - the one place where the random logic lives.
 *
 * Written in the C subset that both C++17 and OpenCL C 1.2 accept: fixed-size
 * arrays, plain structs, no STL, no references, no recursion, no allocation.
 * The CPU wrappers in rules/ and the OpenCL kernel (compute/kernels/scan.cl) both include this
 * file, so the two backends cannot drift apart.
 *
 * Pipeline simulated here:
 *   MT19937 word -> mapping (31-bit rnd()) -> pick rule (index) -> model (rooms)
 *
 * Draw buffers: callers precompute rnd() outputs [0, W) for one seed once, then
 * every offset, gap and event reads from that buffer.
 */
#ifndef SEEDTRACKER_CORE_SIM_CORE_H
#define SEEDTRACKER_CORE_SIM_CORE_H

#ifdef __OPENCL_C_VERSION__
typedef uint uint32_t;
typedef ulong uint64_t;
#define SF_INLINE static inline
#define SF_GLOBAL __global
#define SF_PRIVATE __private
#define SF_CONSTANT __constant
#else
#include <stdint.h>
#define SF_INLINE static inline
#define SF_GLOBAL
#define SF_PRIVATE
#define SF_CONSTANT
#endif

/* ---- constants ---------------------------------------------------------- */

#define SF_MT_N 624u
#define SF_MT_M 397u

#define SF_MAP_HIGH31 0   /* w >> 1                         */
#define SF_MAP_LOW31 1    /* w & 0x7fffffff                 */
#define SF_MAP_REJECT31 2 /* draw until w <= 0x7fffffff     */
#define SF_MAP_RAW 3      /* the 32-bit MT word itself (buffer slots are engine words) */

#define SF_PICK_DIRECT 0      /* raw % choices                                   */
#define SF_PICK_FIXED_MOD 1   /* fixed arithmetic domain, stepped value % choices */
#define SF_PICK_FIXED_SCALE 2 /* fixed arithmetic domain, ordinal scaled to buckets */
#define SF_PICK_DIST_GCC10 3  /* std::uniform_int_distribution(0, choices-1) as in libstdc++ 10;
                                 reads RAW engine words (use with SF_MAP_RAW)             */
#define SF_PICK_DIST_GCC11 4  /* same distribution as libstdc++ 11 (Lemire's method); RAW words */

#define SF_MODEL_REMAINING 0
#define SF_MODEL_REROLL 1
#define SF_MODEL_SWAPREMOVE 2
#define SF_MODEL_SHUFFLE 3
#define SF_MODEL_ROLL 4 /* independent picks of 1..6, no survivor */

#define SF_REROLL_MAX_PICKS 64u /* reroll model: more picks than this marks the event invalid */
#define SF_DIST_MAX_WORDS 64u   /* dist picks: more rejected engine words than this marks the event invalid */

/* ---- MT19937 with on-demand twist --------------------------------------- */

typedef struct {
    uint32_t mt[SF_MT_N];
    uint32_t idx; /* next word to twist, cycles 0..623 */
} sf_mt_state;

/* Fill the first n init words (n <= 624) exactly like std::mt19937's seeding. */
SF_INLINE void sf_mt_init_n(SF_PRIVATE sf_mt_state* s, uint32_t seed, uint32_t n)
{
    uint32_t i;
    s->mt[0] = seed;
    for (i = 1u; i < n; ++i) {
        s->mt[i] = 1812433253u * (s->mt[i - 1u] ^ (s->mt[i - 1u] >> 30)) + i;
    }
    s->idx = 0u;
}

SF_INLINE void sf_mt_init(SF_PRIVATE sf_mt_state* s, uint32_t seed)
{
    sf_mt_init_n(s, seed, SF_MT_N);
}

/*
 * Twist one word in place, then temper it. Word i uses mt[i], mt[i+1] and
 * mt[i+397] (indices mod 624). For i >= 227 the i+397 word and, at i = 623,
 * the mt[0] word were already replaced earlier in the same pass, which is
 * exactly what the block twist of std::mt19937 does. The cycle simply
 * continues into the next block.
 */
SF_INLINE uint32_t sf_mt_next(SF_PRIVATE sf_mt_state* s)
{
    uint32_t i = s->idx;
    uint32_t i1 = (i + 1u == SF_MT_N) ? 0u : i + 1u;
    uint32_t im = i + SF_MT_M;
    uint32_t y, v;
    if (im >= SF_MT_N) im -= SF_MT_N;
    y = (s->mt[i] & 0x80000000u) | (s->mt[i1] & 0x7fffffffu);
    v = s->mt[im] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
    s->mt[i] = v;
    s->idx = i1;
    v ^= v >> 11;
    v ^= (v << 7) & 0x9d2c5680u;
    v ^= (v << 15) & 0xefc60000u;
    v ^= v >> 18;
    return v;
}

/* ---- mappings ----------------------------------------------------------- */

SF_INLINE uint32_t sf_rnd(SF_PRIVATE sf_mt_state* s, int mapping)
{
    uint32_t w = sf_mt_next(s);
    if (mapping == SF_MAP_HIGH31) return w >> 1;
    if (mapping == SF_MAP_LOW31) return w & 0x7fffffffu;
    if (mapping == SF_MAP_RAW) return w;
    while (w > 0x7fffffffu) w = sf_mt_next(s);
    return w;
}

/* Fill out[0..W) with the next W rnd() outputs. */
SF_INLINE void sf_fill_draws(SF_PRIVATE sf_mt_state* s, int mapping, SF_PRIVATE uint32_t* out, uint32_t W)
{
    uint32_t i;
    for (i = 0u; i < W; ++i) out[i] = sf_rnd(s, mapping);
}

/*
 * Seed and fill in one call. For the one-word-per-draw mappings only the init
 * words the first W outputs can touch are computed: output i < 227 needs init
 * words i, i+1 and i+397, so min(624, W+397) words are enough. reject31 draws a
 * variable number of words, so it always gets the full init.
 */
SF_INLINE void sf_seed_fill(SF_PRIVATE sf_mt_state* s, uint32_t seed, int mapping,
                            SF_PRIVATE uint32_t* out, uint32_t W)
{
    uint32_t n = SF_MT_N;
    if (mapping != SF_MAP_REJECT31 && W + SF_MT_M < SF_MT_N) n = W + SF_MT_M;
    sf_mt_init_n(s, seed, n);
    sf_fill_draws(s, mapping, out, W);
}

/*
 * Stateless variant of sf_seed_fill for small windows, used by the GPU kernel
 * (the 624-word state array would live in slow per-thread memory there).
 *
 * The first 227 MT outputs only read init words, never twisted ones: output i
 * needs init words i, i+1 and i+397. The init words form one sequential chain,
 * so this walks the chain once, keeps only words 0..K (scratch, K+1 entries)
 * and streams words 397.. through the twist as it reaches them. Results are
 * bit-identical to sf_seed_fill.
 *
 * K is the number of raw MT words it may consume (K <= 227). For the
 * one-word-per-draw mappings K = W. For reject31 about half the words are
 * dropped, so K is a budget; if W draws are not found within K words the call
 * returns 0 and the caller falls back to sf_mt_init + sf_fill_draws.
 */
#define SF_LAZY_MAX_RAW 227u
#define SF_LAZY_REJECT_MAX_W 96u

/* Raw-word budget K for the lazy fill, or 0 when it does not apply. */
SF_INLINE uint32_t sf_lazy_raw_words(int mapping, uint32_t W)
{
    uint32_t k;
    if (mapping != SF_MAP_REJECT31) return (W != 0u && W + SF_MT_M < SF_MT_N) ? W : 0u;
    if (W == 0u || W > SF_LAZY_REJECT_MAX_W) return 0u;
    k = 2u * W + 32u;
    return k < SF_LAZY_MAX_RAW ? k : SF_LAZY_MAX_RAW;
}

SF_INLINE int sf_seed_fill_lazy(uint32_t seed, int mapping, SF_PRIVATE uint32_t* out, uint32_t W,
                                uint32_t K, SF_PRIVATE uint32_t* scratch)
{
    uint32_t x = seed; /* invariant: x == init word j */
    uint32_t j, i;
    uint32_t got = 0u;
    for (j = 0u; j <= K; ++j) {
        scratch[j] = x;
        x = 1812433253u * (x ^ (x >> 30)) + j + 1u;
    }
    for (j = K + 1u; j < SF_MT_M; ++j) x = 1812433253u * (x ^ (x >> 30)) + j + 1u;
    /* now x == init word 397, the i+397 word of output 0 */
    for (i = 0u; i < K; ++i) {
        uint32_t y = (scratch[i] & 0x80000000u) | (scratch[i + 1u] & 0x7fffffffu);
        uint32_t v = x ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        v ^= v >> 11;
        v ^= (v << 7) & 0x9d2c5680u;
        v ^= (v << 15) & 0xefc60000u;
        v ^= v >> 18;
        if (mapping == SF_MAP_HIGH31) {
            out[i] = v >> 1;
        } else if (mapping == SF_MAP_LOW31) {
            out[i] = v & 0x7fffffffu;
        } else if (mapping == SF_MAP_RAW) {
            out[i] = v;
        } else if (v <= 0x7fffffffu) {
            out[got++] = v;
            if (got == W) return 1;
        }
        x = 1812433253u * (x ^ (x >> 30)) + (SF_MT_M + i + 1u);
    }
    return mapping != SF_MAP_REJECT31;
}

/* ---- pick rules and models ---------------------------------------------- */

typedef struct {
    int model;
    int pick;
    uint32_t rng_step;     /* buffer slots consumed per pick: 1 used + (rng_step-1) skipped */
    uint32_t value_step;   /* fixed picks: domain hop                                       */
    uint32_t domain_count; /* fixed picks: span / value_step + 1, computed once             */
} sf_params;

typedef struct {
    int valid; /* 0 when the event ran past the buffer or hit the reroll cap */
    int elim[5];
    int survivor;
    uint32_t draws_used; /* buffer slots consumed, including skipped rng_step slots */
} sf_event;

/*
 * One pick: reads buf[*pos], advances *pos by rng_step, returns an index in
 * [0, choices). choices <= 1 consumes nothing. Reading at or past W clears *ok.
 */
SF_INLINE int sf_pick(SF_PRIVATE const uint32_t* buf, uint32_t W, SF_PRIVATE uint32_t* pos,
                      SF_PRIVATE const sf_params* p, int choices, SF_PRIVATE int* ok)
{
    uint32_t raw;
    uint32_t ordinal;
    if (choices <= 1) return 0;
    if (p->pick == SF_PICK_DIST_GCC10) {
        /*
         * libstdc++ 10 uniform_int_distribution downscaling, urngrange = 2^32-1:
         *   scaling = urngrange / urange+1; past = (urange+1) * scaling;
         *   draw until w < past; result = w / scaling.
         * Every engine word read, rejected or not, advances *pos by rng_step.
         */
        uint32_t scaling = 0xffffffffu / (uint32_t)choices;
        uint32_t past = (uint32_t)choices * scaling;
        uint32_t words = 0u;
        do {
            if (words >= SF_DIST_MAX_WORDS || *pos >= W) {
                *ok = 0;
                return 0;
            }
            raw = buf[*pos];
            *pos += p->rng_step;
            ++words;
        } while (raw >= past);
        return (int)(raw / scaling);
    }
    if (p->pick == SF_PICK_DIST_GCC11) {
        /*
         * Ported from libstdc++ 11 uniform_int_distribution::operator() for a
         * 32-bit engine (urngrange = 2^32-1), the _S_nd path (Lemire's nearly
         * divisionless method, https://arxiv.org/abs/1805.10941):
         *   product = (u64)g() * range; low = (u32)product;
         *   if (low < range) { threshold = -range % range;
         *                      while (low < threshold) { product = (u64)g() * range; low = (u32)product; } }
         *   result = product >> 32.
         * range = choices. Every engine word read, rejected or not, advances *pos
         * by rng_step; reading more than SF_DIST_MAX_WORDS marks the event invalid.
         */
        uint32_t range = (uint32_t)choices;
        uint32_t words = 0u;
        uint64_t product;
        uint32_t low;
        if (*pos >= W) {
            *ok = 0;
            return 0;
        }
        product = (uint64_t)buf[*pos] * (uint64_t)range;
        *pos += p->rng_step;
        ++words;
        low = (uint32_t)product;
        if (low < range) {
            uint32_t threshold = (0u - range) % range;
            while (low < threshold) {
                if (words >= SF_DIST_MAX_WORDS || *pos >= W) {
                    *ok = 0;
                    return 0;
                }
                product = (uint64_t)buf[*pos] * (uint64_t)range;
                *pos += p->rng_step;
                ++words;
                low = (uint32_t)product;
            }
        }
        return (int)(product >> 32);
    }
    if (*pos >= W) {
        *ok = 0;
        return 0;
    }
    raw = buf[*pos];
    *pos += p->rng_step;

    if (p->pick == SF_PICK_DIRECT) {
        return (int)(raw % (uint32_t)choices);
    }
    ordinal = raw % p->domain_count;
    if (p->pick == SF_PICK_FIXED_MOD) {
        return (int)((ordinal * p->value_step) % (uint32_t)choices);
    }
    {
        int idx = (int)(((uint64_t)ordinal * (uint64_t)choices) / (uint64_t)p->domain_count);
        if (idx >= choices) idx = choices - 1;
        return idx;
    }
}

/* Simulate one full event reading buf[pos...]. */
SF_INLINE void sf_step_event(SF_PRIVATE const uint32_t* buf, uint32_t W, uint32_t pos,
                             SF_PRIVATE const sf_params* p, SF_PRIVATE sf_event* out)
{
    uint32_t cur = pos;
    int ok = 1;
    int i, j;
    out->valid = 0;
    out->survivor = 0;
    out->draws_used = 0u;
    for (i = 0; i < 5; ++i) out->elim[i] = 0;

    if (p->model == SF_MODEL_REMAINING || p->model == SF_MODEL_SWAPREMOVE) {
        int rooms[6];
        int n = 6;
        for (i = 0; i < 6; ++i) rooms[i] = i + 1;
        for (i = 0; i < 5; ++i) {
            int idx = sf_pick(buf, W, &cur, p, n, &ok);
            if (!ok) return;
            out->elim[i] = rooms[idx];
            if (p->model == SF_MODEL_REMAINING) {
                for (j = idx; j < n - 1; ++j) rooms[j] = rooms[j + 1]; /* stable shift */
            } else {
                rooms[idx] = rooms[n - 1];
            }
            --n;
        }
        out->survivor = rooms[0];
    } else if (p->model == SF_MODEL_REROLL) {
        int used[7];
        uint32_t picks = 0u;
        for (i = 0; i < 7; ++i) used[i] = 0;
        for (i = 0; i < 5; ++i) {
            int x;
            do {
                int idx;
                if (picks >= SF_REROLL_MAX_PICKS) return;
                idx = sf_pick(buf, W, &cur, p, 6, &ok);
                if (!ok) return;
                ++picks;
                x = idx + 1;
            } while (used[x]);
            used[x] = 1;
            out->elim[i] = x;
        }
        for (i = 1; i <= 6; ++i) {
            if (!used[i]) {
                out->survivor = i;
                break;
            }
        }
    } else if (p->model == SF_MODEL_ROLL) {
        for (i = 0; i < 5; ++i) {
            int idx = sf_pick(buf, W, &cur, p, 6, &ok);
            if (!ok) return;
            out->elim[i] = idx + 1;
        }
    } else {
        /* SF_MODEL_SHUFFLE: Fisher-Yates, first five slots are the elimination order. */
        int rooms[6];
        for (i = 0; i < 6; ++i) rooms[i] = i + 1;
        for (i = 5; i > 0; --i) {
            int k = sf_pick(buf, W, &cur, p, i + 1, &ok);
            int t;
            if (!ok) return;
            t = rooms[i];
            rooms[i] = rooms[k];
            rooms[k] = t;
        }
        for (i = 0; i < 5; ++i) out->elim[i] = rooms[i];
        out->survivor = rooms[5];
    }
    out->draws_used = cur - pos;
    out->valid = 1;
}

#endif /* SEEDTRACKER_CORE_SIM_CORE_H */
