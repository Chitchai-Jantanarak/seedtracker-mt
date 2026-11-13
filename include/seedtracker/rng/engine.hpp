#pragma once
// SakrayRng: the std::mt19937 + uniform_int_distribution reference engine.
// Used for --mapping std (library-specific, so it has no sim_core equivalent)
// and as the ground truth when checking sim_core. Behaviour is unchanged from
// the legacy tool.
#include <cstdint>
#include <limits>
#include <random>
#include <utility>

#include "seedtracker/types.hpp"

namespace seedtracker {

class SakrayRng {
public:
    SakrayRng(uint32_t seed, Mapping mapping)
        : gen_(seed), mapping_(mapping), dist_(0, std::numeric_limits<int32_t>::max()) {}

    uint32_t rnd() {
        switch (mapping_) {
            case Mapping::StdDistribution:
                return static_cast<uint32_t>(dist_(gen_));
            case Mapping::High31:
                return gen_() >> 1;
            case Mapping::Low31:
                return gen_() & 0x7fffffffU;
            case Mapping::Raw:
                return gen_();
            case Mapping::Reject31: {
                uint32_t x;
                do { x = gen_(); } while (x > 0x7fffffffU);
                return x;
            }
        }
        return 0;
    }

    int script_rand(int minv, int maxv) {
        if (maxv < minv) std::swap(maxv, minv);
        const uint64_t range =
            static_cast<uint64_t>(static_cast<int64_t>(maxv) -
                                  static_cast<int64_t>(minv)) + 1ULL;
        if (range <= 1) return minv;
        const uint64_t r = static_cast<uint64_t>(rnd());
        return static_cast<int>(static_cast<int64_t>(minv) +
                                static_cast<int64_t>(r % range));
    }

    void discard_rnd_calls(uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) (void)rnd();
    }

private:
    std::mt19937 gen_;
    Mapping mapping_;
    std::uniform_int_distribution<int32_t> dist_;
};

}  // namespace seedtracker
