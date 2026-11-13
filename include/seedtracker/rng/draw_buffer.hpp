#pragma once
// DrawBuffer: the rnd() outputs [0, W) of one seed, computed once and read by
// every offset, gap path and event. Replaces the per-offset RNG state copy of
// the legacy tool. One DrawBuffer per worker thread.
#include <cstdint>
#include <vector>

#include "seedtracker/core/sim_core.h"
#include "seedtracker/rng/engine.hpp"
#include "seedtracker/types.hpp"

namespace seedtracker {

class DrawBuffer {
public:
    DrawBuffer(Mapping mapping, uint32_t window) : mapping_(mapping), data_(window) {}

    void fill(uint32_t seed) {
        const uint32_t w = static_cast<uint32_t>(data_.size());
        if (mapping_ == Mapping::StdDistribution) {
            SakrayRng rng(seed, mapping_);
            for (uint32_t i = 0; i < w; ++i) data_[i] = rng.rnd();
        } else {
            sf_seed_fill(&state_, seed, sim_mapping(mapping_), data_.data(), w);
        }
    }

    const uint32_t* data() const { return data_.data(); }
    uint32_t size() const { return static_cast<uint32_t>(data_.size()); }

private:
    Mapping mapping_;
    std::vector<uint32_t> data_;
    sf_mt_state state_{};
};

}  // namespace seedtracker
