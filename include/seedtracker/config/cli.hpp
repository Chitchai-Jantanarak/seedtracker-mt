#pragma once
// Command line: flag parsing and the --help text.
#include <iosfwd>

#include "seedtracker/config/options.hpp"

namespace seedtracker {

// Parses argv into Options (including loading --obs). Per-flag errors throw
// std::runtime_error with a message that names the flag. Does not call
// validate(); the caller does, so it can print the warnings.
Options parse_args(int argc, char** argv);

void usage(std::ostream& os, const char* argv0);

}  // namespace seedtracker
