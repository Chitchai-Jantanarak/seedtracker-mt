#pragma once
// probe_std_mapping(): which explicit mapping does --mapping std equal on this
// build? Compares std::uniform_int_distribution<int32_t>(0, INT32_MAX) over
// std::mt19937 against high31, low31 and reject31 for several seeds.
#include <optional>
#include <string>

#include "seedfinder/types.hpp"

namespace seedfinder {

struct MappingProbeResult {
    std::optional<Mapping> match;  // the explicit mapping std equals on this build, if any
    std::string report;            // human-readable summary, one line per fact
};

// Draws `draws_per_seed` values for each probe seed (default 100000).
MappingProbeResult probe_std_mapping(unsigned draws_per_seed = 100000);

}  // namespace seedfinder
