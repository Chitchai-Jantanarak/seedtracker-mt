#pragma once
// Human-readable summary of a scan: counts, next-elimination distribution,
// survivor distribution, top continuation paths, examples, and (for multi-event
// logs) the volatility tables.
#include <iosfwd>

#include "seedfinder/compute/stats.hpp"
#include "seedfinder/config/options.hpp"

namespace seedfinder {

void print_stats(std::ostream& os, const Stats& s, const Options& opt);

}  // namespace seedfinder
