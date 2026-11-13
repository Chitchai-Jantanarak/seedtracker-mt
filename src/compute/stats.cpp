#include "seedtracker/compute/stats.hpp"

#include <algorithm>

#include "seedtracker/compute/volatility.hpp"

namespace seedtracker {

void init_stats(Stats& s, const Options& opt) {
    s = Stats{};
    s.depth_counts.assign(static_cast<std::size_t>(opt.depth), {});
    const std::size_t keys = pow7(path_length(predicted_prefix_len(opt), opt.depth));
    s.path_counts.assign(keys, 0);
    s.path_weights.assign(keys, 0);
}

namespace {

// Keeps the `limit` smallest hits under `less`, sorted.
void insert_bounded(std::vector<ChainHit>& v, const ChainHit& h, std::size_t limit,
                    bool (*less)(const ChainHit&, const ChainHit&)) {
    if (limit == 0) return;
    if (v.size() >= limit && !less(h, v.back())) return;
    auto pos = std::upper_bound(v.begin(), v.end(), h, less);
    v.insert(pos, h);
    if (v.size() > limit) v.pop_back();
}

}  // namespace

void stats_add(Stats& s, const ChainHit& h, const Options& opt) {
    ++s.matches;
    const std::size_t k = predicted_prefix_len(opt);
    for (int d = 0; d < opt.depth; ++d) {
        const std::size_t pos = k + static_cast<std::size_t>(d);
        if (pos < h.elim.size()) ++s.depth_counts[static_cast<std::size_t>(d)][h.elim[pos]];
    }
    ++s.survivor_counts[h.survivor];
    const uint32_t key = prediction_key(h.elim.data(), k, opt.depth);
    ++s.path_counts[key];
    s.path_weights[key] += h.weight_q;
    if (h.gap_count > 0) s.has_gaps = true;
    insert_bounded(s.examples, h, opt.examples, hit_less);
    if (h.gap_count > 0) insert_bounded(s.lowest_volatility, h, opt.examples, hit_volatility_less);
}

void stats_merge(Stats& dst, const Stats& src, const Options& opt) {
    dst.matches += src.matches;
    dst.invalid_events += src.invalid_events;
    for (int d = 0; d < opt.depth; ++d) {
        for (int v = 1; v <= 6; ++v) {
            dst.depth_counts[static_cast<std::size_t>(d)][static_cast<std::size_t>(v)] +=
                src.depth_counts[static_cast<std::size_t>(d)][static_cast<std::size_t>(v)];
        }
    }
    for (int v = 1; v <= 6; ++v) dst.survivor_counts[static_cast<std::size_t>(v)] += src.survivor_counts[static_cast<std::size_t>(v)];
    for (std::size_t i = 0; i < src.path_counts.size(); ++i) {
        dst.path_counts[i] += src.path_counts[i];
        dst.path_weights[i] += src.path_weights[i];
    }
    dst.has_gaps = dst.has_gaps || src.has_gaps;
    dst.examples.insert(dst.examples.end(), src.examples.begin(), src.examples.end());
    dst.lowest_volatility.insert(dst.lowest_volatility.end(), src.lowest_volatility.begin(),
                                 src.lowest_volatility.end());
}

void stats_finalize(Stats& s, const Options& opt) {
    std::sort(s.examples.begin(), s.examples.end(), hit_less);
    if (s.examples.size() > opt.examples) s.examples.resize(opt.examples);
    std::sort(s.lowest_volatility.begin(), s.lowest_volatility.end(), hit_volatility_less);
    if (s.lowest_volatility.size() > opt.examples) s.lowest_volatility.resize(opt.examples);
}

}  // namespace seedtracker
