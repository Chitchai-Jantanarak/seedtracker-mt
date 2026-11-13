#pragma once
// Stats: what a scan (or a CSV re-filter) accumulates from surviving hits.
// Everything is integer-valued so merged results do not depend on how the work
// was split between threads.
#include <array>
#include <cstdint>
#include <vector>

#include "seedtracker/config/options.hpp"
#include "seedtracker/types.hpp"

namespace seedtracker {

struct Stats {
    uint64_t matches = 0;
    uint64_t invalid_events = 0;  // reroll pick cap or window exceeded
    std::vector<std::array<uint64_t, 7>> depth_counts;  // [depth][value 1..6]
    std::array<uint64_t, 7> survivor_counts{};
    std::vector<uint64_t> path_counts;   // indexed by base-7 continuation key
    std::vector<uint64_t> path_weights;  // same index; sum of weight_q (1 - volatility)
    std::vector<ChainHit> examples;      // first opt.examples hits by (seed, offset, gaps)
    std::vector<ChainHit> lowest_volatility;  // opt.examples hits with the lowest volatility
    bool has_gaps = false;               // some hit came from a multi-event chain
};

void init_stats(Stats& s, const Options& opt);
void stats_add(Stats& s, const ChainHit& h, const Options& opt);
// Merges src into dst. Call stats_finalize on the final result.
void stats_merge(Stats& dst, const Stats& src, const Options& opt);
// Sorts and trims the example lists so output is independent of thread timing.
void stats_finalize(Stats& s, const Options& opt);

}  // namespace seedtracker
