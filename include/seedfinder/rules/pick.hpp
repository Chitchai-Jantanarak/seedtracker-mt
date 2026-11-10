#pragma once
// C++ view of the pick rules (direct, fixed-mod, fixed-scale). The rules
// themselves live in core/sim_core.h; this file maps the CLI enum onto them.
#include "seedfinder/config/options.hpp"
#include "seedfinder/core/sim_core.h"

namespace seedfinder {

inline int sim_pick_rule(PickRule p) {
    switch (p) {
        case PickRule::DirectRemaining: return SF_PICK_DIRECT;
        case PickRule::FixedModulo: return SF_PICK_FIXED_MOD;
        case PickRule::FixedScale: return SF_PICK_FIXED_SCALE;
        case PickRule::DistGcc10: return SF_PICK_DIST_GCC10;
        case PickRule::DistGcc11: return SF_PICK_DIST_GCC11;
    }
    return SF_PICK_DIRECT;
}

}  // namespace seedfinder
