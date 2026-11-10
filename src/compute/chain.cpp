#include "seedfinder/compute/chain.hpp"

#include <stdexcept>
#include <utility>

#include "seedfinder/rng/draw_buffer.hpp"

namespace seedfinder {

ChainMatcher::ChainMatcher(const Options& opt)
    : params_(make_sim_params(opt)),
      steps_(opt.observations),
      offset_begin_(opt.offset_begin),
      offset_end_(opt.offset_end),
      offset_step_(opt.offset_step),
      prefix_len_(predicted_prefix_len(opt)),
      depth_(opt.depth),
      window_(static_cast<uint32_t>(draw_window(opt))) {}

std::vector<Observation> simulate_chain(const Options& opt, uint32_t seed, uint64_t offset,
                                        const std::vector<uint32_t>& gaps) {
    uint64_t w = offset + (gaps.size() + 1) * max_slots_per_event(opt);
    for (uint32_t g : gaps) w += g;
    if (w > kMaxWindow) throw std::runtime_error("offset plus gaps needs too many buffered draws");
    if (gaps.size() + 1 > kMaxEvents) {
        throw std::runtime_error("at most " + std::to_string(kMaxEvents) + " events are supported");
    }

    DrawBuffer buf(effective_mapping(opt), static_cast<uint32_t>(w));
    buf.fill(seed);
    const sf_params params = make_sim_params(opt);

    std::vector<Observation> out;
    uint32_t pos = static_cast<uint32_t>(offset);
    for (std::size_t k = 0; k <= gaps.size(); ++k) {
        if (k > 0) pos += gaps[k - 1];
        const sf_event ev = simulate_event(buf.data(), buf.size(), pos, params);
        if (!ev.valid) {
            throw std::runtime_error("event " + std::to_string(k + 1) +
                                     " is invalid (reroll pick cap or window exceeded)");
        }
        Observation o;
        for (int i = 0; i < 5; ++i) o.rule.prefix.push_back(ev.elim[i]);
        o.rule.survivor = ev.survivor;
        if (k > 0) o.gap_min = o.gap_max = gaps[k - 1];
        out.push_back(std::move(o));
        pos += ev.draws_used;
    }
    return out;
}

}  // namespace seedfinder
