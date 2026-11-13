#include "seedtracker/compute/opencl_scanner.hpp"

#include <CL/cl.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "seedtracker/compute/chain.hpp"
#include "seedtracker/kernel_source.hpp"
#include "seedtracker/rng/draw_buffer.hpp"
#include "seedtracker/rules/model.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace seedtracker {

namespace {

// The OpenCL runtime is loaded at run time instead of linked. A machine with no
// OpenCL driver then gets a normal "OpenCL unavailable" reason (so --backend auto
// can fall back to the CPU) instead of an exe that fails to start. Linking
// System32/OpenCL.dll directly also breaks under a 32-bit-hosted MinGW toolchain
// (TDM-GCC): its ld is a 32-bit process, so Windows hands it the 32-bit
// SysWOW64 copy of the DLL.
#define SF_CL_FUNCTIONS(X)                                                                       X(clGetPlatformIDs) X(clGetDeviceIDs) X(clGetDeviceInfo) X(clCreateContext)                  X(clReleaseContext) X(clCreateCommandQueue) X(clReleaseCommandQueue)                         X(clCreateProgramWithSource) X(clBuildProgram) X(clGetProgramBuildInfo)                      X(clReleaseProgram) X(clCreateKernel) X(clReleaseKernel) X(clSetKernelArg)                   X(clCreateBuffer) X(clReleaseMemObject) X(clEnqueueFillBuffer)                               X(clEnqueueNDRangeKernel) X(clEnqueueReadBuffer) X(clFlush) X(clReleaseEvent)                \
    X(clGetEventProfilingInfo)

struct ClApi {
#define SF_CL_MEMBER(name) decltype(&name) p_##name = nullptr;
    SF_CL_FUNCTIONS(SF_CL_MEMBER)
#undef SF_CL_MEMBER
};
ClApi g_cl;

// Loads the runtime once. Returns false and sets `why` when it is missing.
bool load_opencl(std::string& why) {
    static bool tried = false;
    static bool ok = false;
    static std::string reason;
    if (!tried) {
        tried = true;
#ifdef _WIN32
        HMODULE lib = LoadLibraryA("OpenCL.dll");
        auto sym = [&](const char* n) { return lib ? reinterpret_cast<void*>(GetProcAddress(lib, n)) : nullptr; };
        const char* lib_name = "OpenCL.dll";
#else
        void* lib = dlopen("libOpenCL.so.1", RTLD_NOW);
        auto sym = [&](const char* n) { return lib ? dlsym(lib, n) : nullptr; };
        const char* lib_name = "libOpenCL.so.1";
#endif
        if (!lib) {
            reason = std::string(lib_name) + " not found";
        } else {
            ok = true;
#define SF_CL_LOAD(name)                                                          g_cl.p_##name = reinterpret_cast<decltype(&name)>(sym(#name));                if (!g_cl.p_##name && ok) {                                                       ok = false;                                                                   reason = std::string(lib_name) + " lacks " #name;                         }
            SF_CL_FUNCTIONS(SF_CL_LOAD)
#undef SF_CL_LOAD
        }
    }
    if (!ok) why = reason;
    return ok;
}

}  // namespace

// From here on the cl* names call through the loaded table.
#define clGetPlatformIDs (g_cl.p_clGetPlatformIDs)
#define clGetDeviceIDs (g_cl.p_clGetDeviceIDs)
#define clGetDeviceInfo (g_cl.p_clGetDeviceInfo)
#define clCreateContext (g_cl.p_clCreateContext)
#define clReleaseContext (g_cl.p_clReleaseContext)
#define clCreateCommandQueue (g_cl.p_clCreateCommandQueue)
#define clReleaseCommandQueue (g_cl.p_clReleaseCommandQueue)
#define clCreateProgramWithSource (g_cl.p_clCreateProgramWithSource)
#define clBuildProgram (g_cl.p_clBuildProgram)
#define clGetProgramBuildInfo (g_cl.p_clGetProgramBuildInfo)
#define clReleaseProgram (g_cl.p_clReleaseProgram)
#define clCreateKernel (g_cl.p_clCreateKernel)
#define clReleaseKernel (g_cl.p_clReleaseKernel)
#define clSetKernelArg (g_cl.p_clSetKernelArg)
#define clCreateBuffer (g_cl.p_clCreateBuffer)
#define clReleaseMemObject (g_cl.p_clReleaseMemObject)
#define clEnqueueFillBuffer (g_cl.p_clEnqueueFillBuffer)
#define clEnqueueNDRangeKernel (g_cl.p_clEnqueueNDRangeKernel)
#define clEnqueueReadBuffer (g_cl.p_clEnqueueReadBuffer)
#define clFlush (g_cl.p_clFlush)
#define clReleaseEvent (g_cl.p_clReleaseEvent)
#define clGetEventProfilingInfo (g_cl.p_clGetEventProfilingInfo)

uint64_t gpu_window(const Options& opt) {
    const uint64_t full = draw_window(opt);
    if (full == 0) return 0;
    // Slots one event realistically uses: 5 picks, or about 9 on average (24 at
    // the far tail) for reroll. The CPU window assumes the worst case instead.
    // A custom model is sized at its declared maximum (SF_CUSTOM_MAX_PICKS).
    const uint64_t typical_pick = opt.model == Model::RerollUnique ? 24
                                  : opt.model == Model::Custom     ? SF_CUSTOM_MAX_PICKS
                                                                   : 5;
    const uint64_t typical_event = typical_pick * opt.rng_step;
    uint64_t w = opt.offset_end;
    for (std::size_t i = 1; i < opt.observations.size(); ++i) w += opt.observations[i].gap_max;
    w += opt.observations.size() * typical_event;
    return std::min(w, full);
}

std::string gpu_option_limit(const Options& opt, Mapping gpu_mapping) {
    const uint64_t w = gpu_window(opt);
    const uint32_t limit = gpu_window_limit(gpu_mapping);
    if (w == 0 || w > limit) {
        return "per-seed draw window is " + (w == 0 ? std::string("over the global cap") : std::to_string(w)) +
               " slots, the GPU limit for " + mapping_name(gpu_mapping) + " is " + std::to_string(limit);
    }
    return {};
}

namespace {

const char* cl_error_name(cl_int rc) {
    switch (rc) {
        case CL_SUCCESS: return "CL_SUCCESS";
        case CL_DEVICE_NOT_FOUND: return "CL_DEVICE_NOT_FOUND";
        case CL_OUT_OF_RESOURCES: return "CL_OUT_OF_RESOURCES";
        case CL_OUT_OF_HOST_MEMORY: return "CL_OUT_OF_HOST_MEMORY";
        case CL_MEM_OBJECT_ALLOCATION_FAILURE: return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
        case CL_BUILD_PROGRAM_FAILURE: return "CL_BUILD_PROGRAM_FAILURE";
        case CL_INVALID_VALUE: return "CL_INVALID_VALUE";
        case CL_INVALID_WORK_GROUP_SIZE: return "CL_INVALID_WORK_GROUP_SIZE";
        case CL_INVALID_KERNEL_ARGS: return "CL_INVALID_KERNEL_ARGS";
        case CL_INVALID_BUFFER_SIZE: return "CL_INVALID_BUFFER_SIZE";
        default: return "see cl.h";
    }
}

void check(cl_int rc, const char* what) {
    if (rc != CL_SUCCESS) {
        throw std::runtime_error(std::string("OpenCL ") + what + " failed: " + cl_error_name(rc) + " (" +
                                 std::to_string(rc) + ")");
    }
}

// Releases an OpenCL handle on scope exit. `Releaser::call` wraps the cl*Release function.
template <class T, class Releaser>
struct Handle {
    T h = nullptr;
    Handle() = default;
    explicit Handle(T v) : h(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h(o.h) { o.h = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            reset();
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    ~Handle() { reset(); }
    void reset() {
        if (h) Releaser::call(h);
        h = nullptr;
    }
    operator T() const { return h; }
};
struct ReleaseContext { static void call(cl_context c) { clReleaseContext(c); } };
struct ReleaseQueue { static void call(cl_command_queue q) { clReleaseCommandQueue(q); } };
struct ReleaseProgram { static void call(cl_program p) { clReleaseProgram(p); } };
struct ReleaseKernel { static void call(cl_kernel k) { clReleaseKernel(k); } };
struct ReleaseBuffer { static void call(cl_mem m) { clReleaseMemObject(m); } };
using Context = Handle<cl_context, ReleaseContext>;
using Queue = Handle<cl_command_queue, ReleaseQueue>;
using Program = Handle<cl_program, ReleaseProgram>;
using Kernel = Handle<cl_kernel, ReleaseKernel>;
using Buffer = Handle<cl_mem, ReleaseBuffer>;

std::string join_values(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + v[i];
    return s;
}

// The compile-time half of the kernel's inputs. Everything the kernel needs
// that depends on the options is a constant here, so the private draw buffer
// has a fixed size and the rule checks are constant-array lookups.
std::string build_prelude(const Options& opt, Mapping gpu_mapping) {
    const sf_params p = make_sim_params(opt);
    const std::size_t n = opt.observations.size();
    std::vector<std::string> gmin, gmax, plen, prefix, incl, excl;
    for (std::size_t k = 0; k < n; ++k) {
        const Observation& o = opt.observations[k];
        gmin.push_back(std::to_string(k == 0 ? 0u : o.gap_min) + "u");
        gmax.push_back(std::to_string(k == 0 ? 0u : o.gap_max) + "u");
        plen.push_back(std::to_string(o.rule.prefix.size()));
        std::vector<std::string> elems;
        for (std::size_t i = 0; i < 5; ++i) {
            elems.push_back(std::to_string(i < o.rule.prefix.size() ? o.rule.prefix[i] : 0));
        }
        prefix.push_back("{" + join_values(elems) + "}");
        incl.push_back(std::to_string(o.rule.include_mask) + "u");
        excl.push_back(std::to_string(o.rule.exclude_mask) + "u");
    }
    std::ostringstream o;
    o << "#define SF_K_W " << gpu_window(opt) << "u\n"
      << "#define SF_K_EXACT " << (gpu_window(opt) == draw_window(opt) ? 1 : 0) << "\n"
      << "#define SF_K_EVENTS " << n << "\n"
      << "#define SF_K_MAPPING " << sim_mapping(gpu_mapping) << "\n"
      << "#define SF_K_LAZY_K " << sf_lazy_raw_words(sim_mapping(gpu_mapping), static_cast<uint32_t>(gpu_window(opt))) << "u\n"
      << "#define SF_K_MODEL " << p.model << "\n"
      << "#define SF_K_PICK " << p.pick << "\n"
      << "#define SF_K_RNG_STEP " << p.rng_step << "u\n"
      << "#define SF_K_VALUE_STEP " << p.value_step << "u\n"
      << "#define SF_K_DOMAIN_COUNT " << p.domain_count << "u\n"
      << "#define SF_K_OFFSET_BEGIN " << opt.offset_begin << "u\n"
      << "#define SF_K_OFFSET_END " << opt.offset_end << "u\n"
      << "#define SF_K_OFFSET_STEP " << opt.offset_step << "u\n"
      << "#define SF_K_GAP_MIN_INIT {" << join_values(gmin) << "}\n"
      << "#define SF_K_GAP_MAX_INIT {" << join_values(gmax) << "}\n"
      << "#define SF_K_PREFIX_LEN_INIT {" << join_values(plen) << "}\n"
      << "#define SF_K_PREFIX_INIT {" << join_values(prefix) << "}\n"
      << "#define SF_K_INCLUDE_INIT {" << join_values(incl) << "}\n"
      << "#define SF_K_EXCLUDE_INIT {" << join_values(excl) << "}\n";
    return o.str();
}

// prelude + sim_core.h + scan.cl. scan.cl's #include line is replaced by the sim_core.h text,
// and sim_core.h's own custom_rule.h include line by the custom_rule.h text.
std::string build_source(const Options& opt, Mapping gpu_mapping) {
    std::string sim_core = kSimCoreSource;
    const std::string custom_line = "#include \"seedtracker/core/custom_rule.h\"";
    const std::size_t custom_at = sim_core.find(custom_line);
    if (custom_at == std::string::npos) {
        throw std::runtime_error("sim_core.h lost its custom_rule.h include line");
    }
    sim_core.replace(custom_at, custom_line.size(), kCustomRuleSource);

    std::string kernel = kScanKernelSource;
    const std::string include_line = "#include \"seedtracker/core/sim_core.h\"";
    const std::size_t at = kernel.find(include_line);
    if (at == std::string::npos) throw std::runtime_error("scan.cl lost its sim_core.h include line");
    kernel.replace(at, include_line.size(), sim_core);
    return build_prelude(opt, gpu_mapping) + kernel;
}

std::string device_string(cl_device_id d, cl_device_info what) {
    std::size_t n = 0;
    if (clGetDeviceInfo(d, what, 0, nullptr, &n) != CL_SUCCESS || n == 0) return {};
    std::string s(n, '\0');
    if (clGetDeviceInfo(d, what, n, &s[0], nullptr) != CL_SUCCESS) return {};
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

// A seed the GPU wants the CPU to look at, and whether the CPU's invalid-event
// count for it replaces the GPU's (uncertain seeds, see scan.cl).
struct Recheck {
    uint32_t seed;
    bool uncertain;
};

// Re-runs the CPU matcher on `seeds` (ascending). Seeds are split into
// contiguous slices, one per thread; each slice buffers its CSV rows and the
// slices are written in order, so the file stays in ascending seed order with
// each seed's rows contiguous. Returns the invalid events the CPU saw on the
// uncertain seeds.
uint64_t recheck_on_cpu(const Options& opt, const std::vector<Recheck>& seeds, std::vector<Stats>& locals,
                        CsvWriter* csv) {
    if (seeds.empty()) return 0;
    const std::size_t slices = std::min<std::size_t>(locals.size(), (seeds.size() + 255) / 256);
    std::vector<std::string> rows(csv ? slices : 0);
    std::vector<uint64_t> invalid(slices, 0);
    std::vector<std::thread> threads;
    std::exception_ptr failure;
    std::mutex failure_mutex;
    try {
        for (std::size_t t = 0; t < slices; ++t) {
            const std::size_t from = seeds.size() * t / slices;
            const std::size_t to = seeds.size() * (t + 1) / slices;
            threads.emplace_back([&, t, from, to]() {
                try {
                    ChainMatcher matcher(opt);
                    DrawBuffer draws(effective_mapping(opt), matcher.window());
                    auto on_hit = [&](const ChainHit& h) {
                        stats_add(locals[t], h, opt);
                        if (csv) append_csv_row(rows[t], h);
                    };
                    for (std::size_t i = from; i < to; ++i) {
                        const uint64_t before = matcher.invalid_events();
                        draws.fill(seeds[i].seed);
                        matcher.scan_seed(draws.data(), seeds[i].seed, on_hit);
                        if (seeds[i].uncertain) invalid[t] += matcher.invalid_events() - before;
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                }
            });
        }
    } catch (...) {
        // Creating a thread failed; joinable threads must not be destroyed.
        for (std::thread& th : threads) th.join();
        throw;
    }
    for (std::thread& th : threads) th.join();
    if (failure) std::rethrow_exception(failure);
    if (csv) {
        for (const std::string& r : rows) {
            if (!r.empty()) csv->write_block(r);
        }
    }
    uint64_t total = 0;
    for (uint64_t v : invalid) total += v;
    return total;
}

constexpr std::size_t kHitCapMax = std::size_t{1} << 22;  // hit buffer entries per slot
constexpr uint32_t kInvalidOverflow = 0x80000000u;        // counter this high may have wrapped
constexpr uint64_t kFirstAutoBatch = uint64_t{1} << 16;   // size of the timing launch
constexpr uint64_t kMinAutoBatch = uint64_t{1} << 14;
constexpr double kTargetLaunchSeconds = 0.4;              // well under the ~2 s display watchdog
constexpr std::size_t kCounters = 3;                      // hits, invalid events, uncertain seeds

}  // namespace

struct OpenClScanner::Impl {
    std::string device_name;
    cl_device_id device = nullptr;
    Context context;
    Queue compute_queue;  // kernel launches
    Queue xfer_queue;     // result reads, so they never wait behind the next launch
    Program program;
    Kernel kernel;
    std::size_t hit_cap = 0;

    // One in-flight batch: its buffers and the launch event.
    struct Slot {
        Buffer counters;   // [0] hit count, [1] invalid events, [2] uncertain count
        Buffer hits;
        Buffer uncertain;  // seeds the CPU must settle itself (short window only)
        cl_event done = nullptr;
        uint64_t begin = 0;
        uint64_t count = 0;
        ~Slot() {
            if (done) clReleaseEvent(done);
        }
    };

    struct Result {
        bool overflow = false;
        double kernel_seconds = 0.0;  // device time of the launch
        std::vector<Recheck> seeds;  // ascending by seed
        uint64_t invalid = 0;        // counted on the GPU, uncertain seeds excluded
    };

    void init_slot(Slot& s, std::size_t cap) {
        cl_int rc = CL_SUCCESS;
        s.counters.reset();
        s.hits.reset();
        s.uncertain.reset();
        s.counters = Buffer(clCreateBuffer(context, CL_MEM_READ_WRITE, kCounters * sizeof(cl_uint), nullptr, &rc));
        check(rc, "clCreateBuffer(counters)");
        s.hits = Buffer(clCreateBuffer(context, CL_MEM_READ_WRITE, cap * sizeof(cl_uint), nullptr, &rc));
        check(rc, "clCreateBuffer(hits)");
        s.uncertain = Buffer(clCreateBuffer(context, CL_MEM_READ_WRITE, cap * sizeof(cl_uint), nullptr, &rc));
        check(rc, "clCreateBuffer(uncertain)");
    }

    void submit(Slot& s, uint64_t begin, uint64_t count) {
        if (s.done) {
            clReleaseEvent(s.done);
            s.done = nullptr;
        }
        s.begin = begin;
        s.count = count;
        const cl_uint zero = 0;
        check(clEnqueueFillBuffer(compute_queue, s.counters, &zero, sizeof zero, 0, kCounters * sizeof(cl_uint), 0,
                                  nullptr, nullptr),
              "clEnqueueFillBuffer");
        const cl_uint seed_base = static_cast<cl_uint>(begin);
        const cl_uint n = static_cast<cl_uint>(count);
        const cl_uint cap = static_cast<cl_uint>(std::min<uint64_t>(hit_cap, count));
        cl_mem hits_mem = s.hits;
        cl_mem uncertain_mem = s.uncertain;
        cl_mem counters_mem = s.counters;
        check(clSetKernelArg(kernel, 0, sizeof seed_base, &seed_base), "clSetKernelArg(0)");
        check(clSetKernelArg(kernel, 1, sizeof n, &n), "clSetKernelArg(1)");
        check(clSetKernelArg(kernel, 2, sizeof hits_mem, &hits_mem), "clSetKernelArg(2)");
        check(clSetKernelArg(kernel, 3, sizeof uncertain_mem, &uncertain_mem), "clSetKernelArg(3)");
        check(clSetKernelArg(kernel, 4, sizeof cap, &cap), "clSetKernelArg(4)");
        check(clSetKernelArg(kernel, 5, sizeof counters_mem, &counters_mem), "clSetKernelArg(5)");
        const std::size_t global = static_cast<std::size_t>(count);
        check(clEnqueueNDRangeKernel(compute_queue, kernel, 1, nullptr, &global, nullptr, 0, nullptr, &s.done),
              "clEnqueueNDRangeKernel");
        clFlush(compute_queue);
    }

    // Waits for the launch, reads the counters and, unless a counter overflowed, the seed lists.
    Result collect(Slot& s) {
        Result r;
        cl_uint c[kCounters] = {0, 0, 0};
        check(clEnqueueReadBuffer(xfer_queue, s.counters, CL_TRUE, 0, sizeof c, c, 1, &s.done, nullptr),
              "clEnqueueReadBuffer(counters)");
        cl_ulong t_start = 0, t_end = 0;
        if (clGetEventProfilingInfo(s.done, CL_PROFILING_COMMAND_START, sizeof t_start, &t_start, nullptr) ==
                CL_SUCCESS &&
            clGetEventProfilingInfo(s.done, CL_PROFILING_COMMAND_END, sizeof t_end, &t_end, nullptr) == CL_SUCCESS &&
            t_end >= t_start) {
            r.kernel_seconds = static_cast<double>(t_end - t_start) * 1e-9;
        }
        clReleaseEvent(s.done);
        s.done = nullptr;
        const std::size_t cap = static_cast<std::size_t>(std::min<uint64_t>(hit_cap, s.count));
        if (c[0] > cap || c[2] > cap || c[1] >= kInvalidOverflow) {
            r.overflow = true;
            return r;
        }
        r.invalid = c[1];
        std::vector<uint32_t> hit_seeds(c[0]), uncertain_seeds(c[2]);
        if (c[0] > 0) {
            check(clEnqueueReadBuffer(xfer_queue, s.hits, CL_TRUE, 0, c[0] * sizeof(cl_uint), hit_seeds.data(), 0,
                                      nullptr, nullptr),
                  "clEnqueueReadBuffer(hits)");
        }
        if (c[2] > 0) {
            check(clEnqueueReadBuffer(xfer_queue, s.uncertain, CL_TRUE, 0, c[2] * sizeof(cl_uint),
                                      uncertain_seeds.data(), 0, nullptr, nullptr),
                  "clEnqueueReadBuffer(uncertain)");
        }
        r.seeds.reserve(hit_seeds.size() + uncertain_seeds.size());
        for (uint32_t v : hit_seeds) r.seeds.push_back({v, false});
        for (uint32_t v : uncertain_seeds) r.seeds.push_back({v, true});
        std::sort(r.seeds.begin(), r.seeds.end(),
                  [](const Recheck& a, const Recheck& b) { return a.seed < b.seed; });
        return r;
    }
};

OpenClScanner::OpenClScanner(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
OpenClScanner::~OpenClScanner() = default;

const std::string& OpenClScanner::device_name() const { return impl_->device_name; }

std::unique_ptr<OpenClScanner> OpenClScanner::create(const Options& opt, Mapping gpu_mapping,
                                                     std::string& why_not) {
    if (!load_opencl(why_not)) return nullptr;
    try {
            cl_uint nplat = 0;
            cl_int rc = clGetPlatformIDs(0, nullptr, &nplat);
            if (rc != CL_SUCCESS || nplat == 0) {
                why_not = "no OpenCL platform found";
                return nullptr;
            }
            std::vector<cl_platform_id> platforms(nplat);
            check(clGetPlatformIDs(nplat, platforms.data(), nullptr), "clGetPlatformIDs");
            // Best GPU across all platforms: a discrete card (memory not shared with
            // the host) beats an integrated one, then more compute units x clock.
            cl_device_id device = nullptr;
            unsigned long long best_score = 0;
            for (cl_platform_id plat : platforms) {
                cl_uint nd = 0;
                if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 0, nullptr, &nd) != CL_SUCCESS || nd == 0) continue;
                std::vector<cl_device_id> devs(nd);
                if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, nd, devs.data(), nullptr) != CL_SUCCESS) continue;
                for (cl_device_id d : devs) {
                    cl_bool unified = CL_TRUE;
                    cl_uint units = 1, mhz = 1;
                    clGetDeviceInfo(d, CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof unified, &unified, nullptr);
                    clGetDeviceInfo(d, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof units, &units, nullptr);
                    clGetDeviceInfo(d, CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof mhz, &mhz, nullptr);
                    const unsigned long long score =
                        (unified ? 1ull : (1ull << 40)) + static_cast<unsigned long long>(units) * mhz;
                    if (!device || score > best_score) {
                        device = d;
                        best_score = score;
                    }
                }
            }
            if (!device) {
                why_not = "no OpenCL GPU device found";
                return nullptr;
            }

            auto impl = std::make_unique<Impl>();
            impl->device = device;
            impl->device_name = device_string(device, CL_DEVICE_NAME);
            if (impl->device_name.empty()) impl->device_name = "unknown GPU";

            impl->context = Context(clCreateContext(nullptr, 1, &device, nullptr, nullptr, &rc));
            check(rc, "clCreateContext");
            impl->compute_queue = Queue(clCreateCommandQueue(impl->context, device, CL_QUEUE_PROFILING_ENABLE, &rc));
            check(rc, "clCreateCommandQueue");
            impl->xfer_queue = Queue(clCreateCommandQueue(impl->context, device, 0, &rc));
            check(rc, "clCreateCommandQueue");

            const std::string source = build_source(opt, gpu_mapping);
            const char* src = source.c_str();
            const std::size_t len = source.size();
            impl->program = Program(clCreateProgramWithSource(impl->context, 1, &src, &len, &rc));
            check(rc, "clCreateProgramWithSource");
            rc = clBuildProgram(impl->program, 1, &device, "-cl-std=CL1.2", nullptr, nullptr);
            if (rc != CL_SUCCESS) {
                std::size_t n = 0;
                clGetProgramBuildInfo(impl->program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &n);
                std::string log(n, '\0');
                if (n) clGetProgramBuildInfo(impl->program, device, CL_PROGRAM_BUILD_LOG, n, &log[0], nullptr);
                while (!log.empty() && (log.back() == '\0' || log.back() == '\n')) log.pop_back();
                why_not = "kernel build failed (" + std::string(cl_error_name(rc)) + ")" +
                          (log.empty() ? std::string() : ":\n" + log);
                return nullptr;
            }
            impl->kernel = Kernel(clCreateKernel(impl->program, "sf_scan", &rc));
            check(rc, "clCreateKernel");
            return std::unique_ptr<OpenClScanner>(new OpenClScanner(std::move(impl)));
        } catch (const std::exception& e) {
            why_not = e.what();
            return nullptr;
        }
    }

    Stats OpenClScanner::scan(const Options& opt, CsvWriter* csv) {
        Impl& g = *impl_;
        const uint64_t total = opt.seed_end - opt.seed_begin;
        // --gpu-batch is the launch size when given, and otherwise the ceiling of
        // the automatic sizing below.
        const uint64_t max_batch = std::min<uint64_t>(opt.gpu_batch, total);
        g.hit_cap = static_cast<std::size_t>(std::min<uint64_t>(max_batch, kHitCapMax));

        // A launch that runs for seconds can trip the OS display watchdog (Windows
        // resets a GPU that does not answer within about 2 s). Unless --gpu-batch
        // was given, start small, time each launch on the device and size the next
        // so it takes about kTargetLaunchSeconds, never above max_batch.
        uint64_t batch = opt.gpu_batch_given ? max_batch : std::min<uint64_t>(max_batch, kFirstAutoBatch);

        std::vector<Stats> locals(std::max(1u, opt.threads));
        for (Stats& s : locals) init_stats(s, opt);
        uint64_t invalid_total = 0;
        uint64_t hit_total = 0;
        uint64_t batches = 0;
        double wait_seconds = 0.0;
        double kernel_seconds = 0.0;
        double recheck_seconds = 0.0;
        using clock = std::chrono::steady_clock;

        // Re-checking runs on CPU threads while the host thread waits on the GPU, so
        // the GPU works on batch i+1 during the re-check of batch i. Only one
        // re-check runs at a time, which keeps the CSV in ascending seed order.
        struct RecheckDone {
            uint64_t invalid;
            double seconds;
        };
        std::future<RecheckDone> pending;
        bool has_pending = false;
        auto finish_pending = [&]() {
            if (!has_pending) return;
            const RecheckDone d = pending.get();
            has_pending = false;
            invalid_total += d.invalid;
            recheck_seconds += d.seconds;
        };
        auto recheck_job = [&](std::vector<Recheck> seeds) {
            const auto t0 = clock::now();
            const uint64_t invalid = recheck_on_cpu(opt, seeds, locals, csv);
            return RecheckDone{invalid, std::chrono::duration<double>(clock::now() - t0).count()};
        };
        auto consume_async = [&](Impl::Result&& r) {
            invalid_total += r.invalid;
            hit_total += r.seeds.size();
            finish_pending();
            pending = std::async(std::launch::async, recheck_job, std::move(r.seeds));
            has_pending = true;
        };
        auto consume_sync = [&](Impl::Result&& r) {
            invalid_total += r.invalid;
            hit_total += r.seeds.size();
            finish_pending();
            const RecheckDone d = recheck_job(std::move(r.seeds));
            invalid_total += d.invalid;
            recheck_seconds += d.seconds;
        };
        auto retune = [&](const Impl::Result& r, uint64_t launched) {
            kernel_seconds += r.kernel_seconds;
            if (opt.gpu_batch_given || r.kernel_seconds <= 0.0) return;
            const double rate = static_cast<double>(launched) / r.kernel_seconds;  // seeds per second
            const double want = rate * kTargetLaunchSeconds;
            // Grow at most 4x per step so one cheap launch cannot cause a long one.
            const double capped = std::min(want, static_cast<double>(launched) * 4.0);
            // max_batch can be below kMinAutoBatch on a small scan; clamp needs lo <= hi.
            batch = std::clamp<uint64_t>(static_cast<uint64_t>(capped), std::min(kMinAutoBatch, max_batch), max_batch);
        };

        // The spare slot runs overflowed batches again, synchronously, as halves.
        Impl::Slot spare;
        g.init_slot(spare, g.hit_cap);
        std::function<void(uint64_t, uint64_t)> run_halves = [&](uint64_t begin, uint64_t count) {
            if (count < 2) throw std::runtime_error("GPU counters overflowed on a single seed");
            const uint64_t first = count / 2;
            const uint64_t parts[2][2] = {{begin, first}, {begin + first, count - first}};
            for (const auto& part : parts) {
                g.submit(spare, part[0], part[1]);
                Impl::Result r = g.collect(spare);
                ++batches;
                kernel_seconds += r.kernel_seconds;
                if (r.overflow) run_halves(part[0], part[1]);
                else consume_sync(std::move(r));
            }
        };

        Impl::Slot slots[2];
        for (Impl::Slot& s : slots) g.init_slot(s, g.hit_cap);

        uint64_t next = opt.seed_begin;
        auto launch = [&](Impl::Slot& s) {
            const uint64_t n = std::min<uint64_t>(batch, opt.seed_end - next);
            g.submit(s, next, n);
            next += n;
        };

        int cur = 0;
        launch(slots[cur]);
        for (;;) {
            const int other = 1 - cur;
            const bool more = next < opt.seed_end;
            if (more) launch(slots[other]);  // the GPU starts on it while the host re-checks `cur`

            const uint64_t launched = slots[cur].count;
            const auto t0 = clock::now();
            Impl::Result r = g.collect(slots[cur]);
            wait_seconds += std::chrono::duration<double>(clock::now() - t0).count();
            ++batches;
            retune(r, launched);
            if (r.overflow) {
                finish_pending();
                run_halves(slots[cur].begin, slots[cur].count);
            } else {
                consume_async(std::move(r));
            }

            if (!more) break;
            cur = other;
        }

        finish_pending();

        std::cerr << "gpu: " << batches << " launches (" << (opt.gpu_batch_given ? "fixed at " : "auto-sized, up to ")
                  << (opt.gpu_batch_given ? batch : max_batch) << " seeds), " << hit_total
                  << " seeds re-checked on cpu; kernel " << kernel_seconds << " s, waited " << wait_seconds
                  << " s for the GPU, " << recheck_seconds << " s re-checking (overlapped)" << std::endl;

        Stats all;
        init_stats(all, opt);
        for (const Stats& s : locals) stats_merge(all, s, opt);
        // The kernel walks the same tree as the CPU matcher for every seed, so its
        // count (plus the CPU's count on uncertain seeds) is the whole-scan total.
        all.invalid_events = invalid_total;
        stats_finalize(all, opt);
        return all;
    }

    }  // namespace seedtracker
