#pragma once
// Calibration volatility: how much the prediction for the last logged event
// depends on the unknown gap in front of it.
//
// For one (seed, start offset, earlier gap path) the matcher finds every value
// of the last gap whose event still matches the log. Each such gap predicts a
// continuation (the next `depth` unobserved eliminations). Then
//
//   volatility = 1 - (count of the most common prediction) / (matching gaps)
//
// 0 means every surviving gap predicts the same continuation; values near 1
// mean the prediction hinges on the gap. A single event has no gap, so its
// volatility is 0.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace seedtracker {

constexpr unsigned kWeightBits = 20;  // fixed-point scale of ChainHit::weight_q

// Number of eliminations a continuation key covers.
inline std::size_t path_length(std::size_t prefix_len, int depth) {
    const std::size_t remaining = prefix_len < 5 ? 5 - prefix_len : 0;
    return std::min<std::size_t>(static_cast<std::size_t>(depth), remaining);
}

inline uint32_t pow7(std::size_t n) {
    uint32_t v = 1;
    for (std::size_t i = 0; i < n; ++i) v *= 7;
    return v;
}

// Base-7 integer of the eliminations after the known prefix (digits 1..6).
template <class E>
inline uint32_t prediction_key(const E* elim, std::size_t prefix_len, int depth) {
    const std::size_t len = path_length(prefix_len, depth);
    uint32_t key = 0;
    for (std::size_t d = 0; d < len; ++d) key = key * 7 + static_cast<uint32_t>(elim[prefix_len + d]);
    return key;
}

struct VolatilityScore {
    uint32_t agree = 1;  // size of the largest group of equal predictions
    uint32_t tried = 1;  // matching gaps
    double volatility = 0.0;
    uint64_t weight_q = uint64_t{1} << kWeightBits;  // (1 - volatility) in fixed point
};

// `keys` holds one prediction key per matching gap (non-empty). Sorts it in place.
VolatilityScore score_predictions(std::vector<uint32_t>& keys);

}  // namespace seedtracker
