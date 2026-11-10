#pragma once
// C++ view of the elimination models (remaining, swapremove, reroll, shuffle).
// The models live in core/sim_core.h; this file builds the sim parameters from
// Options and wraps one event step.
#include "seedfinder/config/options.hpp"
#include "seedfinder/core/sim_core.h"
#include "seedfinder/rules/pick.hpp"

namespace seedfinder {

inline int sim_model(Model m) {
    switch (m) {
        case Model::RemainingErase: return SF_MODEL_REMAINING;
        case Model::RerollUnique: return SF_MODEL_REROLL;
        case Model::SwapRemove: return SF_MODEL_SWAPREMOVE;
        case Model::FisherYates: return SF_MODEL_SHUFFLE;
    }
    return SF_MODEL_REMAINING;
}

// Requires validate() to have filled opt.pick_ctx.
inline sf_params make_sim_params(const Options& opt) {
    sf_params p;
    p.model = sim_model(opt.model);
    p.pick = sim_pick_rule(opt.pick_rule);
    p.rng_step = static_cast<uint32_t>(opt.rng_step);
    p.value_step = static_cast<uint32_t>(opt.value_step);
    p.domain_count = opt.pick_ctx.domain_count;
    return p;
}

inline sf_event simulate_event(const uint32_t* buf, uint32_t size, uint32_t pos, const sf_params& p) {
    sf_event ev;
    sf_step_event(buf, size, pos, &p, &ev);
    return ev;
}

}  // namespace seedfinder
