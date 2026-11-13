#pragma once
// Shared value types: enums, observation records, scan hits.
#include <array>
#include <cstdint>
#include <string>

#include "seedtracker/core/sim_core.h"
#include "seedtracker/post/rules.hpp"

namespace seedtracker {

// Raw is internal (not a CLI value): the unmapped 32-bit MT word, used by the dist-* pick rules.
enum class Mapping { StdDistribution, High31, Low31, Reject31, Raw };
// Which engine runs the scan. Auto picks OpenCL when it is built in, a GPU
// initialises and the options fit; otherwise the CPU thread pool.
enum class Backend { Auto, OpenCL, Cpu };
enum class Model { RemainingErase, RerollUnique, SwapRemove, FisherYates };
enum class PickRule {
    DirectRemaining,  // script_rand(0, choices-1)
    FixedModulo,      // script_rand(base_min, base_max), then modulo choices
    FixedScale,       // script_rand(base_min, base_max), then scale into choices buckets
    DistGcc10,        // std::uniform_int_distribution(0, choices-1) as in libstdc++ 10, on raw MT words
    DistGcc11,        // same as libstdc++ 11 (Lemire's method), on raw MT words
};

inline const char* mapping_name(Mapping m) {
    switch (m) {
        case Mapping::StdDistribution: return "std";
        case Mapping::High31: return "high31";
        case Mapping::Low31: return "low31";
        case Mapping::Reject31: return "reject31";
        case Mapping::Raw: return "raw";
    }
    return "?";
}

inline const char* backend_name(Backend b) {
    switch (b) {
        case Backend::Auto: return "auto";
        case Backend::OpenCL: return "opencl";
        case Backend::Cpu: return "cpu";
    }
    return "?";
}

inline const char* model_name(Model m) {
    switch (m) {
        case Model::RemainingErase: return "remaining";
        case Model::RerollUnique: return "reroll";
        case Model::SwapRemove: return "swapremove";
        case Model::FisherYates: return "shuffle";
    }
    return "?";
}

inline const char* pick_rule_name(PickRule p) {
    switch (p) {
        case PickRule::DirectRemaining: return "direct";
        case PickRule::FixedModulo: return "fixed-mod";
        case PickRule::FixedScale: return "fixed-scale";
        case PickRule::DistGcc10: return "dist-gcc10";
        case PickRule::DistGcc11: return "dist-gcc11";
    }
    return "?";
}

// Explicit mappings only; Std has no sim_core equivalent.
inline int sim_mapping(Mapping m) {
    switch (m) {
        case Mapping::High31: return SF_MAP_HIGH31;
        case Mapping::Low31: return SF_MAP_LOW31;
        case Mapping::Reject31: return SF_MAP_REJECT31;
        case Mapping::Raw: return SF_MAP_RAW;
        case Mapping::StdDistribution: break;
    }
    return -1;
}

constexpr std::size_t kMaxEvents = 8;
constexpr std::size_t kMaxGaps = kMaxEvents - 1;
constexpr uint32_t kMaxGapValue = 1u << 20;

// One logged event. `gap_min..gap_max` is the number of rnd() calls between the
// previous event's last consumed slot and this event's first slot (0..0 for the
// first event).
struct Observation {
    PostRule rule;
    uint32_t gap_min = 0;
    uint32_t gap_max = 0;
};

// One surviving (seed, start offset, gap path) with the predicted (last) event.
struct ChainHit {
    uint32_t seed = 0;
    uint64_t offset = 0;
    std::array<uint32_t, kMaxGaps> gaps{};
    uint8_t gap_count = 0;
    std::array<uint8_t, 5> elim{};
    uint8_t survivor = 0;
    double volatility = 0.0;   // 0 = prediction independent of the unknown last gap
    uint64_t weight_q = 1u << 20;  // (1 - volatility) in 20-bit fixed point
};

inline bool hit_less(const ChainHit& a, const ChainHit& b) {
    if (a.seed != b.seed) return a.seed < b.seed;
    if (a.offset != b.offset) return a.offset < b.offset;
    for (std::size_t i = 0; i < kMaxGaps; ++i) {
        if (a.gaps[i] != b.gaps[i]) return a.gaps[i] < b.gaps[i];
    }
    return false;
}

inline bool hit_volatility_less(const ChainHit& a, const ChainHit& b) {
    if (a.volatility != b.volatility) return a.volatility < b.volatility;
    return hit_less(a, b);
}

}  // namespace seedtracker
