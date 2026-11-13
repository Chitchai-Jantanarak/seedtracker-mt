#pragma once
// OpenClScanner: the GPU backend. The GPU is an exact prefilter, not a second
// implementation of the output: one work-item per seed runs the whole chain
// (compute/kernels/scan.cl, built on core/sim_core.h) and reports the seeds
// where ANY offset and gap path matches. The host then runs the CPU
// ChainMatcher on exactly those seeds, in ascending seed order, so stats, CSV
// rows, volatility and examples come from the same code as the CPU backend.
//
// Seeds go to the GPU in batches (--gpu-batch). If a batch reports more hits
// than the hit buffer holds, it is rerun as two half-size batches; hits are
// never dropped. While the host re-checks batch i, the GPU already runs i+1.
//
// This header has no OpenCL types, so it compiles without the CL headers; the
// implementation is only built with SEEDTRACKER_OPENCL.
#include <memory>
#include <string>

#include "seedtracker/compute/backend.hpp"

namespace seedtracker {

// Draws the kernel fills per seed. The CPU window is sized for the worst case
// (64 words per dist pick, 64 picks per reroll event); the GPU fills only what
// events typically use. Seeds whose events run past that are re-run on the CPU
// (see scan.cl), so the result stays exact. 0 if the CPU window is over its cap.
uint64_t gpu_window(const Options& opt);

// Largest gpu_window the kernel handles well, per mapping. Within it the kernel
// builds the draws without the 624-word MT state, which is what makes the GPU
// faster than the CPU: with the full state in each work-item's private memory
// it measured slower than 8 CPU threads (see README, "GPU window limit").
inline uint32_t gpu_window_limit(Mapping gpu_mapping) {
    return gpu_mapping == Mapping::Reject31 ? SF_LAZY_REJECT_MAX_W : SF_LAZY_MAX_RAW - 1;
}

// Empty when `opt` fits the GPU backend for `gpu_mapping` (the explicit mapping
// the kernel will use), otherwise the reason it does not.
std::string gpu_option_limit(const Options& opt, Mapping gpu_mapping);

class OpenClScanner : public IScanBackend {
public:
    // Opens the first GPU device and builds the kernel for `opt`. `gpu_mapping`
    // is the explicit mapping the kernel fills its window with (for
    // --mapping std, whatever the probe found equal). Returns nullptr and sets
    // `why_not` when no GPU is usable or the kernel does not build.
    static std::unique_ptr<OpenClScanner> create(const Options& opt, Mapping gpu_mapping,
                                                 std::string& why_not);
    ~OpenClScanner() override;

    Stats scan(const Options& opt, CsvWriter* csv) override;

    const std::string& device_name() const;

private:
    struct Impl;
    explicit OpenClScanner(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace seedtracker
