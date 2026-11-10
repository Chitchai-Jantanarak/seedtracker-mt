#include "seedfinder/compute/volatility.hpp"

namespace seedfinder {

VolatilityScore score_predictions(std::vector<uint32_t>& keys) {
    VolatilityScore s;
    const std::size_t n = keys.size();
    if (n <= 1) return s;
    std::sort(keys.begin(), keys.end());
    std::size_t best = 1, run = 1;
    for (std::size_t i = 1; i < n; ++i) {
        run = (keys[i] == keys[i - 1]) ? run + 1 : 1;
        best = std::max(best, run);
    }
    s.agree = static_cast<uint32_t>(best);
    s.tried = static_cast<uint32_t>(n);
    s.volatility = 1.0 - static_cast<double>(best) / static_cast<double>(n);
    s.weight_q = (static_cast<uint64_t>(best) << kWeightBits) / n;
    return s;
}

}  // namespace seedfinder
