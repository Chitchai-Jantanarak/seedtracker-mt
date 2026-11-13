#include "seedtracker/compute/backend_select.hpp"

#include <stdexcept>

#include "seedtracker/compute/cpu_scanner.hpp"
#ifdef SEEDTRACKER_HAVE_OPENCL
#include "seedtracker/compute/opencl_scanner.hpp"
#include "seedtracker/rng/mapping_probe.hpp"
#endif

namespace seedtracker {

namespace {

BackendSelection use_cpu(const Options& opt, std::string note = {}) {
    BackendSelection s;
    s.backend = std::make_unique<CpuScanner>();
    s.description = "cpu (" + std::to_string(opt.threads) + " threads)";
    s.note = std::move(note);
    return s;
}

}  // namespace

BackendSelection select_backend(Options& opt) {
    BackendSelection sel;
    if (opt.backend == Backend::Cpu) {
        sel = use_cpu(opt);
    } else {
#ifndef SEEDTRACKER_HAVE_OPENCL
        if (opt.backend == Backend::OpenCL) {
            throw std::runtime_error(
                "--backend opencl: built without OpenCL (configure with -DSEEDTRACKER_OPENCL=ON, "
                "preset mingw-release-opencl)");
        }
        sel = use_cpu(opt);  // auto in a CPU-only build is simply cpu
#else
        std::string reason;
        std::unique_ptr<OpenClScanner> gpu;
        Mapping gpu_mapping = effective_mapping(opt);
        bool std_resolved = false;
        if (gpu_mapping == Mapping::StdDistribution) {
            // std has no kernel form; use the explicit mapping it equals on this build.
            const MappingProbeResult probe = probe_std_mapping();
            if (probe.match) {
                gpu_mapping = *probe.match;
                std_resolved = true;
            } else {
                reason = "--mapping std matches no explicit mapping on this build (see --probe-mapping)";
            }
        }
        if (reason.empty()) reason = gpu_option_limit(opt, gpu_mapping);
        if (reason.empty()) gpu = OpenClScanner::create(opt, gpu_mapping, reason);
        if (gpu) {
            sel.description = "opencl (" + gpu->device_name() + ")";
            // Only warn when the user asked for std; it is the default mapping, so an
            // unconditional warning would print on every GPU run.
            if (std_resolved && opt.mapping_given) {
                sel.warnings.push_back(std::string("--mapping std on the GPU uses ") + mapping_name(gpu_mapping) +
                                       ", which --probe-mapping found equal to std on this build; "
                                       "the GPU may miss hits if that equality does not hold");
            }
            sel.backend = std::move(gpu);
        } else if (opt.backend == Backend::OpenCL) {
            throw std::runtime_error("--backend opencl: " + reason);
        } else {
            sel = use_cpu(opt, "note: OpenCL unavailable (" + reason + "), using CPU backend");
        }
#endif
    }
    opt.backend_desc = sel.description;
    return sel;
}

}  // namespace seedtracker
