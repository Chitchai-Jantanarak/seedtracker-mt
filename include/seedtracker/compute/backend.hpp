#pragma once
// IScanBackend: a way of running the seed scan. The CPU thread pool is the only
// implementation so far; an OpenCL backend is planned behind the same interface.
#include "seedtracker/compute/stats.hpp"
#include "seedtracker/config/options.hpp"
#include "seedtracker/export/csv.hpp"

namespace seedtracker {

class IScanBackend {
public:
    virtual ~IScanBackend() = default;

    // Scans [seed_begin, seed_end). Writes every survivor to `csv` when it is
    // non-null. Returns the finalized stats. Requires validate() to have run.
    virtual Stats scan(const Options& opt, CsvWriter* csv) = 0;
};

}  // namespace seedtracker
