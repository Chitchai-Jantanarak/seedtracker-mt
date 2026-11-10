#pragma once
// Options: the run configuration (the "setter"). A plain struct filled by the
// CLI, then checked and completed by validate().
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "seedfinder/types.hpp"

namespace seedfinder {

// Values the pick rules would otherwise recompute on every draw.
struct PickContext {
    uint32_t span = 0;          // base_max - base_min
    uint32_t domain_count = 1;  // span / value_step + 1
};

constexpr uint64_t kMaxStep = uint64_t{1} << 20;        // cap for the three --*-step flags
constexpr uint64_t kMaxWindow = uint64_t{1} << 24;      // cap on draws buffered per seed
constexpr uint64_t kDefaultGpuBatch = uint64_t{1} << 24; // seeds per GPU launch

struct Options {
    uint64_t seed_begin = 0;
    uint64_t seed_end = 1u << 20;  // exclusive; small safe default demo shard
    uint64_t offset_begin = 0;
    uint64_t offset_end = 1;       // exclusive
    uint64_t offset_step = 1;      // candidate starts: N, N+step, ...
    uint64_t rng_step = 1;         // used event draws: N, N+step, ...
    uint64_t value_step = 1;       // fixed domain values: min, min+step, ... <= max
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    Backend backend = Backend::Auto;            // --backend auto|opencl|cpu
    uint64_t gpu_batch = kDefaultGpuBatch;      // --gpu-batch: seeds per GPU launch
    bool gpu_batch_given = false;               // when not given, launches are sized automatically (up to gpu_batch)
    int depth = 3;
    std::size_t examples = 12;
    Mapping mapping = Mapping::StdDistribution;
    Model model = Model::RemainingErase;
    PickRule pick_rule = PickRule::DirectRemaining;
    int base_min = 1;
    int base_max = 10000;

    // Logged events, in order; the last one is the event being predicted.
    // --calib fills a single event; --obs reads a file.
    std::vector<Observation> observations;
    std::string obs_file;
    bool calib_given = false;

    std::string csv_out;
    std::string csv_in;

    bool help = false;

    // --emit-events SEED OFFSET GAPS: print a synthetic log and exit.
    bool emit_events = false;
    uint32_t emit_seed = 0;
    uint64_t emit_offset = 0;
    std::vector<uint32_t> emit_gaps;

    // Which flags the user typed (used only for warnings).
    bool base_flags_given = false;  // --base-min, --base-max or --value-step
    bool mapping_given = false;     // --mapping

    // --probe-mapping: report which explicit mapping std equals on this build, then exit.
    bool probe_mapping = false;

    // Derived by validate().
    PickContext pick_ctx;

    // Filled by backend selection: what actually runs, e.g. "cpu (8 threads)".
    std::string backend_desc;
};

// Checks cross-field rules and fills the derived values. Throws
// std::runtime_error on a hard error; returns non-fatal warnings.
std::vector<std::string> validate(Options& opt);

// The draw source actually used. dist-* picks read raw MT words, so the
// mapping flag is ignored and buffer slots (offsets, gaps) count engine words.
inline bool pick_uses_raw_words(PickRule p) {
    return p == PickRule::DistGcc10 || p == PickRule::DistGcc11;
}
inline Mapping effective_mapping(const Options& opt) {
    return pick_uses_raw_words(opt.pick_rule) ? Mapping::Raw : opt.mapping;
}

// Rule of the event being predicted (the last logged event).
const PostRule& predicted_rule(const Options& opt);
std::size_t predicted_prefix_len(const Options& opt);

// Most buffer slots one event can consume.
uint64_t max_slots_per_event(const Options& opt);

// Draw-buffer size per seed: offset_end + sum of gap maxima + events * max slots.
// Never undersized in normal runs; 0 if it would exceed kMaxWindow.
uint64_t draw_window(const Options& opt);

}  // namespace seedfinder
