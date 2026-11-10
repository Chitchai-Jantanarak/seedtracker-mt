#pragma once
// Chooses the scan backend from --backend and what this build and machine can
// do. `auto` (the default) prefers the GPU and falls back to the CPU with a
// note; `opencl` fails instead of falling back; `cpu` never touches the GPU.
#include <memory>
#include <string>
#include <vector>

#include "seedfinder/compute/backend.hpp"
#include "seedfinder/config/options.hpp"

namespace seedfinder {

struct BackendSelection {
    std::unique_ptr<IScanBackend> backend;
    std::string description;            // "opencl (<device>)" or "cpu (N threads)"
    std::vector<std::string> warnings;  // printed as "warning: ..." on stderr
    std::string note;                   // fallback note, printed as-is on stderr; may be empty
};

// Sets opt.backend_desc. Throws std::runtime_error when --backend opencl cannot
// be honoured. Requires validate().
BackendSelection select_backend(Options& opt);

}  // namespace seedfinder
