#include "seedtracker/export/report.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "seedtracker/compute/volatility.hpp"
#include "seedtracker/logs/observation_log.hpp"

namespace seedtracker {

namespace {

// Inverse of prediction_key: "2-4-1", or "(none)" for an empty continuation.
std::string decode_path(uint32_t key, std::size_t len) {
    if (len == 0) return "(none)";
    std::string digits(len, '?');
    for (std::size_t i = len; i-- > 0;) {
        digits[i] = static_cast<char>('0' + key % 7);
        key /= 7;
    }
    std::string out;
    for (std::size_t i = 0; i < len; ++i) {
        if (i) out += '-';
        out += digits[i];
    }
    return out;
}

void print_hit(std::ostream& os, const ChainHit& c, bool with_volatility) {
    os << "  seed=" << c.seed << " offset=" << c.offset;
    if (c.gap_count > 0) {
        os << " gaps=";
        for (std::size_t i = 0; i < c.gap_count; ++i) {
            if (i) os << '-';
            os << c.gaps[i];
        }
    }
    os << " seq=";
    for (std::size_t i = 0; i < 5; ++i) {
        if (i) os << '-';
        os << static_cast<int>(c.elim[i]);
    }
    if (c.survivor) os << " survivor=" << static_cast<int>(c.survivor);
    if (with_volatility) os << " volatility=" << c.volatility;
    os << '\n';
}

}  // namespace

void print_stats(std::ostream& os, const Stats& s, const Options& opt) {
    os << "\n=== Candidate summary ===\n";
    if (!opt.backend_desc.empty()) os << "backend    : " << opt.backend_desc << "\n";
    os << "model      : " << model_name(opt.model) << "\n";
    os << "mapping    : " << mapping_name(effective_mapping(opt));
    if (pick_uses_raw_words(opt.pick_rule)) os << " (--mapping ignored; offsets and gaps count engine words)";
    os << "\n";
    os << "pick rule  : " << pick_rule_name(opt.pick_rule) << "\n";
    if (opt.pick_rule != PickRule::DirectRemaining) {
        os << "base range : " << opt.base_min << ".." << opt.base_max << " step " << opt.value_step << "\n";
    }
    os << "offset step: " << opt.offset_step << "\n";
    os << "rng step   : " << opt.rng_step << "\n";
    os << "calibration: " << summarize_observations(opt.observations) << "\n";
    os << "matching candidates: " << s.matches << "\n";
    if (s.invalid_events) {
        os << "invalid events     : " << s.invalid_events << " (reroll pick cap or draw window exceeded)\n";
    }

    if (!s.matches) {
        os << "No candidates. Expand seed/offset ranges, or try another model/mapping.\n";
        return;
    }

    const std::size_t prefix_len = predicted_prefix_len(opt);
    os << std::fixed << std::setprecision(3);
    for (int d = 0; d < opt.depth; ++d) {
        const std::size_t pos = prefix_len + static_cast<std::size_t>(d);
        if (pos >= 5) break;
        os << "\nNext depth " << (d + 1) << " (" << (model_eliminates(opt.model) ? "elimination" : "roll")
           << " #" << (pos + 1) << "):\n";
        std::vector<std::pair<int, uint64_t>> vv;
        for (int v = 1; v <= 6; ++v) {
            const uint64_t n = s.depth_counts[static_cast<std::size_t>(d)][static_cast<std::size_t>(v)];
            if (n) vv.push_back({v, n});
        }
        std::sort(vv.begin(), vv.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        for (const auto& [v, n] : vv) {
            os << "  " << v << " : " << (100.0 * static_cast<double>(n) / static_cast<double>(s.matches)) << "% ("
               << n << ")\n";
        }
    }

    std::vector<std::pair<int, uint64_t>> sv;
    for (int v = 1; v <= 6; ++v) {
        if (s.survivor_counts[static_cast<std::size_t>(v)]) {
            sv.push_back({v, s.survivor_counts[static_cast<std::size_t>(v)]});
        }
    }
    std::sort(sv.begin(), sv.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (!sv.empty()) os << "\nSurvivor distribution:\n";
    for (const auto& [v, n] : sv) {
        os << "  " << v << " : " << (100.0 * static_cast<double>(n) / static_cast<double>(s.matches)) << "% (" << n
           << ")\n";
    }

    const std::size_t path_len = path_length(prefix_len, opt.depth);
    os << "\nTop continuation paths (up to depth " << opt.depth << "):\n";
    std::vector<std::pair<uint32_t, uint64_t>> paths;
    for (std::size_t key = 0; key < s.path_counts.size(); ++key) {
        if (s.path_counts[key]) paths.push_back({static_cast<uint32_t>(key), s.path_counts[key]});
    }
    std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::size_t shown = 0;
    for (const auto& [key, n] : paths) {
        os << "  " << decode_path(key, path_len) << " : "
           << (100.0 * static_cast<double>(n) / static_cast<double>(s.matches)) << "% (" << n << ")\n";
        if (++shown >= 15) break;
    }

    if (!s.examples.empty()) {
        os << "\nExample surviving candidates:\n";
        for (const ChainHit& c : s.examples) print_hit(os, c, false);
    }

    if (s.has_gaps) {
        os << "\nLowest-volatility survivors (0 = the prediction does not depend on the unknown last gap):\n";
        for (const ChainHit& c : s.lowest_volatility) print_hit(os, c, true);

        uint64_t total = 0;
        for (uint64_t w : s.path_weights) total += w;
        os << "\nVolatility-weighted continuation (each survivor votes with weight 1 - volatility):\n";
        std::vector<std::pair<uint32_t, uint64_t>> weighted;
        for (std::size_t key = 0; key < s.path_weights.size(); ++key) {
            if (s.path_weights[key]) weighted.push_back({static_cast<uint32_t>(key), s.path_weights[key]});
        }
        std::sort(weighted.begin(), weighted.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        shown = 0;
        for (const auto& [key, w] : weighted) {
            os << "  " << decode_path(key, path_len) << " : "
               << (total ? 100.0 * static_cast<double>(w) / static_cast<double>(total) : 0.0) << "% (weight "
               << (static_cast<double>(w) / static_cast<double>(uint64_t{1} << kWeightBits)) << ")\n";
            if (++shown >= 15) break;
        }
    }
}

}  // namespace seedtracker
