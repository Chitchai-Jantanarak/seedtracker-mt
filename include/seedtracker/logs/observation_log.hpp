#pragma once
// Plain-text observation log: reader, writer, and the one-line summary used in
// reports and CSV metadata.
//
//   # comment
//   event 5,1,2,6,3 survivor=4 exclude=2
//   gap 0..40            (or "gap 17" for an exact gap)
//   event 2,4,1
//   event 3              (partial prefix; the last event is the one predicted)
#include <cstddef>
#include <string>
#include <vector>

#include "seedtracker/types.hpp"

namespace seedtracker {

// Parses "5,1,2" into rooms. Throws std::runtime_error naming `what` on bad input.
std::vector<int> parse_room_list(const std::string& s, const std::string& what, std::size_t max_len);

// Throw std::runtime_error with "<name>:<line>: ..." on bad input.
std::vector<Observation> parse_observation_text(const std::string& text, const std::string& name);
std::vector<Observation> read_observation_file(const std::string& path);

// observations.txt-format text for the given events (gaps printed as ranges, or
// exact values when min == max).
std::string format_observation_log(const std::vector<Observation>& obs);

// "5,1" for one plain event, "(none)" for an empty one, otherwise a compact
// "5,1+s4|g0..40|2,4,1" form without spaces.
std::string summarize_observations(const std::vector<Observation>& obs);

}  // namespace seedtracker
