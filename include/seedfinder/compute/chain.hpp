#pragma once
// Gap-tolerant multi-event matching over one seed's draw buffer.
//
// Event 1 starts at each offset in [offset_begin, offset_end) stepping by
// offset_step. It consumes `draws_used` slots. Event k+1 then starts after any
// gap in its `gap a..b` range. A path survives only if every event satisfies
// its post-rule. The last event is the predicted one: for each earlier path the
// matcher collects all last-gap values that still match and scores their
// predictions (see volatility.hpp).
#include <cstdint>
#include <vector>

#include "seedfinder/compute/volatility.hpp"
#include "seedfinder/config/options.hpp"
#include "seedfinder/core/sim_core.h"
#include "seedfinder/post/rules.hpp"
#include "seedfinder/rules/model.hpp"
#include "seedfinder/types.hpp"

namespace seedfinder {

// One matcher per worker thread (it owns scratch buffers). Requires validate().
class ChainMatcher {
public:
    explicit ChainMatcher(const Options& opt);

    uint32_t window() const { return window_; }

    // Events that ran past the buffer or hit the reroll pick cap, so were
    // treated as non-matching.
    uint64_t invalid_events() const { return invalid_; }

    // Scans every offset of one seed; calls emit(const ChainHit&) per survivor,
    // in (offset, gap path) order.
    template <class Emit>
    void scan_seed(const uint32_t* buf, uint32_t seed, Emit&& emit) {
        buf_ = buf;
        seed_ = seed;
        for (uint64_t off = offset_begin_; off < offset_end_; off += offset_step_) {
            start_ = off;
            descend(0, static_cast<uint32_t>(off), emit);
        }
    }

private:
    struct Match {
        uint32_t gap;
        sf_event ev;
    };

    ChainHit make_hit(const sf_event& ev) const {
        ChainHit h;
        h.seed = seed_;
        h.offset = start_;
        for (std::size_t i = 0; i < 5; ++i) h.elim[i] = static_cast<uint8_t>(ev.elim[i]);
        h.survivor = static_cast<uint8_t>(ev.survivor);
        return h;
    }

    template <class Emit>
    void descend(std::size_t k, uint32_t pos, Emit& emit) {
        const sf_event ev = simulate_event(buf_, window_, pos, params_);
        if (!ev.valid) {
            ++invalid_;
            return;
        }
        if (!rule_matches(steps_[k].rule, ev.elim, ev.survivor)) return;
        if (k + 1 == steps_.size()) {  // single event: no gap, volatility 0
            emit(make_hit(ev));
            return;
        }
        const uint32_t end = pos + ev.draws_used;
        const Observation& next = steps_[k + 1];
        if (k + 2 == steps_.size()) {
            last_event(end, next, emit);
            return;
        }
        for (uint32_t g = next.gap_min; g <= next.gap_max; ++g) {
            gaps_[k] = g;
            descend(k + 1, end + g, emit);
        }
    }

    // Tries every gap of the last event, scores the matches, emits them.
    template <class Emit>
    void last_event(uint32_t end, const Observation& last, Emit& emit) {
        const std::size_t gap_slot = steps_.size() - 2;
        matches_.clear();
        keys_.clear();
        for (uint32_t g = last.gap_min; g <= last.gap_max; ++g) {
            const sf_event ev = simulate_event(buf_, window_, end + g, params_);
            if (!ev.valid) {
                ++invalid_;
                continue;
            }
            if (!rule_matches(last.rule, ev.elim, ev.survivor)) continue;
            matches_.push_back({g, ev});
            keys_.push_back(prediction_key(ev.elim, prefix_len_, depth_));
        }
        if (matches_.empty()) return;
        const VolatilityScore score = score_predictions(keys_);
        for (const Match& m : matches_) {
            ChainHit h = make_hit(m.ev);
            for (std::size_t i = 0; i < gap_slot; ++i) h.gaps[i] = gaps_[i];
            h.gaps[gap_slot] = m.gap;
            h.gap_count = static_cast<uint8_t>(gap_slot + 1);
            h.volatility = score.volatility;
            h.weight_q = score.weight_q;
            emit(h);
        }
    }

    sf_params params_;
    std::vector<Observation> steps_;
    uint64_t offset_begin_, offset_end_, offset_step_;
    std::size_t prefix_len_;
    int depth_;
    uint32_t window_;

    const uint32_t* buf_ = nullptr;
    uint32_t seed_ = 0;
    uint64_t start_ = 0;
    uint32_t gaps_[kMaxGaps] = {};
    std::vector<Match> matches_;
    std::vector<uint32_t> keys_;
    uint64_t invalid_ = 0;
};

// Simulates full events from `seed` at `offset` with exactly these gaps (one
// event per gap, plus the first) under the current model/mapping/pick flags,
// and returns them as a log: the input for --emit-events. Throws if an event
// cannot be simulated.
std::vector<Observation> simulate_chain(const Options& opt, uint32_t seed, uint64_t offset,
                                        const std::vector<uint32_t>& gaps);

}  // namespace seedfinder
