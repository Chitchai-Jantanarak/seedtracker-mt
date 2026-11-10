#pragma once
// CpuScanner: worker threads pull 64K-seed chunks from an atomic counter. Each
// worker fills one draw buffer per seed, runs the chain matcher over it, buffers
// its own CSV rows, and keeps private Stats that are merged at the end.
#include "seedfinder/compute/backend.hpp"

namespace seedfinder {

constexpr uint64_t kSeedsPerChunk = 65536;

class CpuScanner : public IScanBackend {
public:
    Stats scan(const Options& opt, CsvWriter* csv) override;
};

}  // namespace seedfinder
