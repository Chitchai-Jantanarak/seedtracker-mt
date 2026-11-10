#include "seedfinder/config/options.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>

namespace seedfinder {

const PostRule& predicted_rule(const Options& opt) { return opt.observations.back().rule; }

std::size_t predicted_prefix_len(const Options& opt) { return predicted_rule(opt).prefix.size(); }

uint64_t max_slots_per_event(const Options& opt) {
    const uint64_t picks = opt.model == Model::RerollUnique ? SF_REROLL_MAX_PICKS : 5;
    // A dist-* pick may read several engine words (rejected draws), up to the cap.
    const uint64_t words = pick_uses_raw_words(opt.pick_rule) ? SF_DIST_MAX_WORDS : 1;
    return picks * words * opt.rng_step;
}

uint64_t draw_window(const Options& opt) {
    if (opt.offset_end > kMaxWindow) return 0;
    uint64_t w = opt.offset_end;
    for (std::size_t i = 1; i < opt.observations.size(); ++i) w += opt.observations[i].gap_max;
    w += opt.observations.size() * max_slots_per_event(opt);
    return w > kMaxWindow ? 0 : w;
}

std::vector<std::string> validate(Options& opt) {
    std::vector<std::string> warnings;
    const bool scanning = opt.csv_in.empty() && !opt.emit_events;

    if (opt.seed_end > (uint64_t{1} << 32)) throw std::runtime_error("seed-end must be <= 2^32");
    if (scanning && opt.seed_begin >= opt.seed_end) throw std::runtime_error("seed-begin must be < seed-end");
    if (scanning && opt.offset_begin >= opt.offset_end) {
        throw std::runtime_error("offset-begin must be < offset-end");
    }
    if (!opt.csv_in.empty() && !opt.csv_out.empty()) {
        // Opening csv-out truncates it, which would destroy the input before it is read.
        std::error_code ec;
        namespace fs = std::filesystem;
        if (fs::exists(opt.csv_out, ec) && fs::exists(opt.csv_in, ec) &&
            fs::equivalent(opt.csv_in, opt.csv_out, ec)) {
            throw std::runtime_error("csv-in and csv-out are the same file: " + opt.csv_in);
        }
    }
    if (opt.base_min > opt.base_max) throw std::runtime_error("base-min must be <= base-max");
    if (opt.base_min < 0 || opt.base_max < 0) {
        throw std::runtime_error("base-min/base-max must be in 0..INT32_MAX");
    }
    if (opt.offset_step < 1 || opt.offset_step > kMaxStep) {
        throw std::runtime_error("offset-step must be in 1..2^20");
    }
    if (opt.rng_step < 1 || opt.rng_step > kMaxStep) throw std::runtime_error("rng-step must be in 1..2^20");
    if (opt.value_step < 1 || opt.value_step > kMaxStep) {
        throw std::runtime_error("value-step must be in 1..2^20");
    }
    if (opt.depth < 1) opt.depth = 1;
    if (opt.depth > 5) opt.depth = 5;
    if (opt.threads < 1) opt.threads = 1;
    // The kernel takes the launch size as a 32-bit count, so 2^32 would wrap to 0.
    if (opt.gpu_batch < 1 || opt.gpu_batch > UINT32_MAX) {
        throw std::runtime_error("gpu-batch must be in 1..4294967295");
    }

    if (opt.observations.empty()) opt.observations.emplace_back();
    if (opt.observations.size() > kMaxEvents) {
        throw std::runtime_error("at most " + std::to_string(kMaxEvents) + " events are supported");
    }

    opt.pick_ctx.span = static_cast<uint32_t>(static_cast<int64_t>(opt.base_max) -
                                              static_cast<int64_t>(opt.base_min));
    opt.pick_ctx.domain_count =
        static_cast<uint32_t>(opt.pick_ctx.span / static_cast<uint32_t>(opt.value_step) + 1u);

    if (scanning && draw_window(opt) == 0) {
        throw std::runtime_error("offset range plus gaps needs more than " + std::to_string(kMaxWindow) +
                                 " buffered draws per seed; lower --offset-end or the gap limits");
    }

    // Warnings (stderr, non-fatal).
    bool all_empty = true;
    for (const Observation& o : opt.observations) all_empty = all_empty && o.rule.empty();
    if (all_empty && (scanning || !opt.csv_in.empty())) {
        warnings.push_back("empty calibration: every event matches, so every seed/offset is a candidate "
                           "(pass --calib or --obs)");
    }
    if (pick_uses_raw_words(opt.pick_rule) && opt.mapping_given) {
        warnings.push_back("--mapping is ignored under --pick " + std::string(pick_rule_name(opt.pick_rule)) +
                           ": it reads raw MT words, so offsets and gaps count engine words, not rnd() calls");
    }
    if (opt.pick_rule == PickRule::DirectRemaining || pick_uses_raw_words(opt.pick_rule)) {
        if (opt.base_flags_given) {
            warnings.push_back("--base-min/--base-max/--value-step are ignored under --pick " +
                               std::string(pick_rule_name(opt.pick_rule)));
        }
    } else {
        if (opt.value_step > opt.pick_ctx.span) {
            warnings.push_back("value-step (" + std::to_string(opt.value_step) +
                               ") is larger than base-max - base-min (" +
                               std::to_string(opt.pick_ctx.span) + "): the fixed domain has one value");
        }
        if (opt.pick_rule == PickRule::FixedModulo && opt.value_step % 2 == 0) {
            warnings.push_back("even value-step with fixed-mod: every stepped value is even, so odd "
                               "remainders are never produced for even choice counts");
        }
    }
    return warnings;
}

}  // namespace seedfinder
