#include "seedtracker/config/cli.hpp"

#include <cstdint>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>

#include "seedtracker/logs/observation_log.hpp"

namespace seedtracker {

namespace {

[[noreturn]] void bad_value(const std::string& flag, const std::string& value, const std::string& why) {
    throw std::runtime_error(flag + ": " + why + " (got '" + value + "')");
}

// Unsigned integer; rejects a leading '-' (std::stoull would wrap it to 2^64-1).
uint64_t parse_u64(const std::string& s, const std::string& flag) {
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) bad_value(flag, s, "expected an unsigned integer");
    if (s[first] == '-') bad_value(flag, s, "must not be negative");
    try {
        std::size_t idx = 0;
        const unsigned long long v = std::stoull(s, &idx, 0);
        if (idx != s.size()) bad_value(flag, s, "expected an unsigned integer");
        return v;
    } catch (const std::invalid_argument&) {
        bad_value(flag, s, "expected an unsigned integer");
    } catch (const std::out_of_range&) {
        bad_value(flag, s, "value out of range");
    }
}

uint64_t parse_step(const std::string& s, const std::string& flag) {
    const uint64_t v = parse_u64(s, flag);
    if (v < 1) bad_value(flag, s, "must be >= 1");
    if (v > kMaxStep) bad_value(flag, s, "must be <= 2^20 (1048576)");
    return v;
}

int parse_base(const std::string& s, const std::string& flag) {
    const uint64_t v = parse_u64(s, flag);
    if (v > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        bad_value(flag, s, "must be in 0..INT32_MAX");
    }
    return static_cast<int>(v);
}

int parse_depth(const std::string& s) {
    try {
        std::size_t idx = 0;
        const int v = std::stoi(s, &idx, 0);
        if (idx != s.size()) bad_value("--depth", s, "expected an integer");
        return v;  // clamped to 1..5 by validate()
    } catch (const std::invalid_argument&) {
        bad_value("--depth", s, "expected an integer");
    } catch (const std::out_of_range&) {
        bad_value("--depth", s, "value out of range");
    }
}

Mapping parse_mapping(const std::string& s) {
    if (s == "std") return Mapping::StdDistribution;
    if (s == "high31") return Mapping::High31;
    if (s == "low31") return Mapping::Low31;
    if (s == "reject31") return Mapping::Reject31;
    throw std::runtime_error("unknown mapping: " + s);
}

Model parse_model(const std::string& s) {
    if (s == "remaining") return Model::RemainingErase;
    if (s == "reroll") return Model::RerollUnique;
    if (s == "swapremove") return Model::SwapRemove;
    if (s == "shuffle") return Model::FisherYates;
    if (s == "roll") return Model::IndependentRoll;
    if (s == "custom") return Model::Custom;
    throw std::runtime_error("unknown model: " + s);
}

PickRule parse_pick_rule(const std::string& s) {
    if (s == "direct") return PickRule::DirectRemaining;
    if (s == "fixed-mod") return PickRule::FixedModulo;
    if (s == "fixed-scale") return PickRule::FixedScale;
    if (s == "dist-gcc10") return PickRule::DistGcc10;
    if (s == "dist-gcc11") return PickRule::DistGcc11;
    if (s == "custom") return PickRule::Custom;
    throw std::runtime_error("unknown pick rule: " + s);
}

Backend parse_backend(const std::string& s) {
    if (s == "auto") return Backend::Auto;
    if (s == "opencl") return Backend::OpenCL;
    if (s == "cpu") return Backend::Cpu;
    throw std::runtime_error("unknown backend: " + s + " (expected auto, opencl or cpu)");
}

std::vector<uint32_t> parse_gap_list(const std::string& s) {
    std::vector<uint32_t> out;
    if (s == "-" || s.empty()) return out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t p = s.find(',', start);
        const std::string part = s.substr(start, p == std::string::npos ? p : p - start);
        const uint64_t v = parse_u64(part, "--emit-events gaps");
        if (v > kMaxGapValue) bad_value("--emit-events gaps", part, "must be <= 2^20");
        out.push_back(static_cast<uint32_t>(v));
        if (p == std::string::npos) return out;
        start = p + 1;
    }
}

}  // namespace

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (a == "--seed-begin") o.seed_begin = parse_u64(need("--seed-begin"), a);
        else if (a == "--seed-end") o.seed_end = parse_u64(need("--seed-end"), a);
        else if (a == "--offset-begin") o.offset_begin = parse_u64(need("--offset-begin"), a);
        else if (a == "--offset-end") o.offset_end = parse_u64(need("--offset-end"), a);
        else if (a == "--offset-step") o.offset_step = parse_step(need("--offset-step"), a);
        else if (a == "--rng-step") o.rng_step = parse_step(need("--rng-step"), a);
        else if (a == "--value-step") { o.value_step = parse_step(need("--value-step"), a); o.base_flags_given = true; }
        else if (a == "--threads") {
            const uint64_t t = parse_u64(need("--threads"), a);
            if (t > 4096) bad_value(a, std::to_string(t), "must be <= 4096");
            o.threads = static_cast<unsigned>(t);
        }
        else if (a == "--backend") o.backend = parse_backend(need("--backend"));
        else if (a == "--gpu-batch") { o.gpu_batch = parse_u64(need("--gpu-batch"), a); o.gpu_batch_given = true; }
        else if (a == "--depth") o.depth = parse_depth(need("--depth"));
        else if (a == "--examples") o.examples = static_cast<std::size_t>(parse_u64(need("--examples"), a));
        else if (a == "--mapping") { o.mapping = parse_mapping(need("--mapping")); o.mapping_given = true; }
        else if (a == "--model") o.model = parse_model(need("--model"));
        else if (a == "--pick") o.pick_rule = parse_pick_rule(need("--pick"));
        else if (a == "--base-min") { o.base_min = parse_base(need("--base-min"), a); o.base_flags_given = true; }
        else if (a == "--base-max") { o.base_max = parse_base(need("--base-max"), a); o.base_flags_given = true; }
        else if (a == "--calib") {
            o.calib_given = true;
            Observation ev;
            ev.rule.prefix = parse_room_list(need("--calib"), "--calib", 5);
            o.observations.assign(1, ev);
        }
        else if (a == "--obs") o.obs_file = need("--obs");
        else if (a == "--probe-mapping") o.probe_mapping = true;
        else if (a == "--csv-out") o.csv_out = need("--csv-out");
        else if (a == "--csv-in") o.csv_in = need("--csv-in");
        else if (a == "--emit-events") {
            const std::string seed = need("--emit-events");
            const std::string offset = need("--emit-events");
            const std::string gaps = need("--emit-events");
            const uint64_t s = parse_u64(seed, "--emit-events SEED");
            if (s > std::numeric_limits<uint32_t>::max()) bad_value("--emit-events SEED", seed, "must be <= UINT32_MAX");
            o.emit_events = true;
            o.emit_seed = static_cast<uint32_t>(s);
            o.emit_offset = parse_u64(offset, "--emit-events OFFSET");
            o.emit_gaps = parse_gap_list(gaps);
        }
        else if (a == "--help" || a == "-h") { o.help = true; return o; }
        else throw std::runtime_error("unknown option: " + a);
    }

    if (!o.obs_file.empty()) {
        if (o.calib_given) throw std::runtime_error("use either --calib or --obs, not both");
        o.observations = read_observation_file(o.obs_file);
    }
    return o;
}

void usage(std::ostream& os, const char* argv0) {
    os <<
        "Usage:\n"
        "  " << argv0 << " [options]\n\n"
        "Scan mode options:\n"
        "  --seed-begin N       inclusive seed start (default 0)\n"
        "  --seed-end N         exclusive seed end (default 2^20 demo shard)\n"
        "  --offset-begin N     inclusive rnd()-call offset (default 0)\n"
        "  --offset-end N       exclusive offset (default 1)\n"
        "  --offset-step N      test starts N,N+step,... (default 1, max 2^20)\n"
        "  --rng-step N         event uses draws N,N+step,... (default 1, max 2^20)\n"
        "  --threads N          CPU worker threads (also re-checks GPU hits)\n"
        "  --backend auto|opencl|cpu\n"
        "                       where to run the scan (default auto): auto uses the GPU when this\n"
        "                       build has OpenCL, a GPU initialises and the options fit its limits,\n"
        "                       and otherwise the CPU with a note on stderr; opencl fails instead\n"
        "                       of falling back; cpu never touches the GPU\n"
        "  --gpu-batch N        seeds per GPU launch. Default: launches are timed and sized to about\n"
        "                       0.4 s each, up to 2^24 seeds (keeps the display watchdog quiet)\n\n"
        "Filtering/prediction:\n"
        "  --calib 5,1,2        observed elimination prefix (one event)\n"
        "  --obs FILE           several logged events with unknown rnd() gaps (see README)\n"
        "  --depth N            aggregate N future eliminations (default 3)\n"
        "  --model remaining|reroll|swapremove|shuffle|roll|custom\n"
        "                       roll: independent rand() % 6 + 1 values\n"
        "                       custom: edit include/seedtracker/core/custom_rule.h and rebuild\n"
        "  --mapping std|high31|low31|reject31\n"
        "  --pick direct|fixed-mod|fixed-scale|dist-gcc10|dist-gcc11|custom\n"
        "                       how each random draw maps to available choices;\n"
        "                       custom: edit include/seedtracker/core/custom_rule.h and rebuild\n"
        "                       dist-gcc10 / dist-gcc11 emulate std::uniform_int_distribution(0,n-1)\n"
        "                       from libstdc++ 10 (rejection) / 11 (Lemire) on RAW MT words\n"
        "                       (--mapping ignored; offsets and gaps then count engine words)\n"
        "  --base-min N         fixed source minimum, 0..INT32_MAX (default 1)\n"
        "  --base-max N         fixed source maximum, 0..INT32_MAX (default 10000)\n"
        "  --value-step N       fixed-domain hop: 1,3,5... => min=1 step=2 (default 1, max 2^20)\n"
        "  --csv-out FILE       save every surviving candidate\n"
        "  --csv-in FILE        re-filter a previously saved candidate CSV\n"
        "  --examples N         examples printed (default 12)\n"
        "  --probe-mapping      print which explicit mapping std equals on this build, then exit\n"
        "  --emit-events SEED OFFSET GAP1,GAP2,...\n"
        "                       print a synthetic observation log for that seed (debug)\n\n"
        "Examples:\n"
        "  # Scan a small shard after observing 5,1:\n"
        "  " << argv0 << " --seed-begin 0 --seed-end 10000000 --offset-begin 0 --offset-end 1 \\\n"
        "      --model remaining --mapping std --pick direct --calib 5,1 --depth 3 --csv-out cand_51.csv\n\n"
        "  # Same elimination model, but a fixed value domain 1..10000; the value's offset from\n"
        "  # --base-min is taken modulo remaining choices (rand(min,max) % n with an unaligned\n"
        "  # min needs --pick custom):\n"
        "  " << argv0 << " --seed-begin 0 --seed-end 10000000 --offset-begin 0 --offset-end 1 \\\n"
        "      --model remaining --mapping std --pick fixed-mod --base-min 1 --base-max 10000 \\\n"
        "      --calib 4,3,6,2,5 --depth 3\n\n"
        "  # Candidate starts 1,3,5,...9999; inside event use N,N+2,N+4...:\n"
        "  " << argv0 << " --offset-begin 1 --offset-end 10000 --offset-step 2 --rng-step 2 \\\n"
        "      --model remaining --mapping std --pick direct --calib 4,3 --depth 3\n\n"
        "  # Sparse fixed VALUE domain 1,3,5,...9999 (single draw per selection):\n"
        "  " << argv0 << " --model remaining --mapping std --pick fixed-scale \\\n"
        "      --base-min 1 --base-max 10000 --value-step 2 --calib 4,3 --depth 3\n\n"
        "  # After the real third elimination is 2, cut candidates without rescanning:\n"
        "  " << argv0 << " --csv-in cand_51.csv --calib 5,1,2 --depth 2 --csv-out cand_512.csv\n\n"
        "  # Several logged events with unknown gaps between them:\n"
        "  " << argv0 << " --seed-end 100000000 --offset-end 8 --obs examples/observations.txt \\\n"
        "      --csv-out chain.csv\n";
}

}  // namespace seedtracker
