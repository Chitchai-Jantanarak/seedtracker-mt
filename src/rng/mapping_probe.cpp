#include "seedfinder/rng/mapping_probe.hpp"

#include <array>
#include <vector>

#include "seedfinder/core/sim_core.h"
#include "seedfinder/rng/engine.hpp"

namespace seedfinder {

MappingProbeResult probe_std_mapping(unsigned draws_per_seed) {
    const std::array<Mapping, 3> candidates = {Mapping::High31, Mapping::Low31, Mapping::Reject31};
    const std::array<uint32_t, 5> seeds = {0u, 1u, 5489u, 123456u, 0xFFFFFFFFu};

    std::array<bool, 3> equal = {true, true, true};
    std::vector<uint32_t> reference(draws_per_seed), other(draws_per_seed);
    sf_mt_state state;
    for (uint32_t seed : seeds) {
        SakrayRng std_rng(seed, Mapping::StdDistribution);
        for (unsigned i = 0; i < draws_per_seed; ++i) reference[i] = std_rng.rnd();
        for (std::size_t c = 0; c < candidates.size(); ++c) {
            sf_seed_fill(&state, seed, sim_mapping(candidates[c]), other.data(), draws_per_seed);
            if (other != reference) equal[c] = false;
        }
    }

    MappingProbeResult r;
    std::string matches;
    for (std::size_t c = 0; c < candidates.size(); ++c) {
        if (!equal[c]) continue;
        if (!r.match) r.match = candidates[c];
        matches += (matches.empty() ? "" : ", ") + std::string(mapping_name(candidates[c]));
    }
    r.report = "std == " + (matches.empty() ? std::string("none") : matches) + "\n";
    r.report += "probed " + std::to_string(seeds.size()) + " seeds x " + std::to_string(draws_per_seed) + " draws\n";
#ifdef _GLIBCXX_RELEASE
    r.report += "libstdc++ release: " + std::to_string(_GLIBCXX_RELEASE) + "\n";
#endif
    if (!r.match) r.report += "no explicit mapping reproduces std here; only the CPU std path is exact\n";
    return r;
}

}  // namespace seedfinder
