#include "seedtracker/export/csv.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

#include "seedtracker/compute/volatility.hpp"
#include "seedtracker/logs/observation_log.hpp"

namespace seedtracker {

namespace {

struct MetaField {
    std::string key;
    std::string value;
};

std::vector<MetaField> current_metadata(const Options& opt) {
    return {
        {"model", model_name(opt.model)},
        {"mapping", mapping_name(effective_mapping(opt))},
        {"pick", pick_rule_name(opt.pick_rule)},
        {"base_min", std::to_string(opt.base_min)},
        {"base_max", std::to_string(opt.base_max)},
        {"value_step", std::to_string(opt.value_step)},
        {"offset_step", std::to_string(opt.offset_step)},
        {"rng_step", std::to_string(opt.rng_step)},
        {"obs", summarize_observations(opt.observations)},
    };
}

void append_uint(std::string& out, unsigned long long v) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
}

bool parse_uint(const std::string& s, unsigned long long& out) {
    if (s.empty() || s.size() > 20) return false;
    unsigned long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const unsigned d = static_cast<unsigned>(c - '0');
        if (v > (std::numeric_limits<unsigned long long>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
}

std::string strip_cr(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

[[noreturn]] void row_error(const std::string& path, std::size_t line_no, const std::string& msg) {
    throw std::runtime_error("csv-in " + path + " line " + std::to_string(line_no) + ": " + msg);
}

ChainHit parse_row(const std::string& line, const std::string& path, std::size_t line_no) {
    const std::vector<std::string> f = split(line, ',');
    if (f.size() != 8 && f.size() != 10) {
        row_error(path, line_no, "expected 8 or 10 columns, found " + std::to_string(f.size()));
    }
    const bool wide = f.size() == 10;
    const std::size_t e0 = wide ? 3 : 2;  // index of e1

    ChainHit h;
    unsigned long long v = 0;
    if (!parse_uint(f[0], v)) row_error(path, line_no, "bad seed '" + f[0] + "'");
    if (v > std::numeric_limits<uint32_t>::max()) {
        row_error(path, line_no, "seed " + f[0] + " is above UINT32_MAX");
    }
    h.seed = static_cast<uint32_t>(v);
    if (!parse_uint(f[1], v)) row_error(path, line_no, "bad offset '" + f[1] + "'");
    h.offset = v;

    for (std::size_t i = 0; i < 5; ++i) {
        const std::string name = "e" + std::to_string(i + 1);
        if (!parse_uint(f[e0 + i], v) || v < 1 || v > 6) {
            row_error(path, line_no, name + " must be 1..6, got '" + f[e0 + i] + "'");
        }
        h.elim[i] = static_cast<uint8_t>(v);
    }
    if (!parse_uint(f[e0 + 5], v) || v < 1 || v > 6) {
        row_error(path, line_no, "survivor must be 1..6, got '" + f[e0 + 5] + "'");
    }
    h.survivor = static_cast<uint8_t>(v);

    if (wide) {
        if (f[2] != "-") {
            const std::vector<std::string> gaps = split(f[2], '-');
            if (gaps.size() > kMaxGaps) row_error(path, line_no, "gap_path has too many gaps");
            for (const std::string& g : gaps) {
                if (!parse_uint(g, v) || v > kMaxGapValue) {
                    row_error(path, line_no, "bad gap_path '" + f[2] + "'");
                }
                h.gaps[h.gap_count++] = static_cast<uint32_t>(v);
            }
        }
        char* end = nullptr;
        const double vol = std::strtod(f[9].c_str(), &end);
        if (f[9].empty() || end != f[9].c_str() + f[9].size() || !(vol >= 0.0 && vol <= 1.0)) {
            row_error(path, line_no, "volatility must be a number in 0..1, got '" + f[9] + "'");
        }
        h.volatility = vol;
        h.weight_q = static_cast<uint64_t>((1.0 - vol) * static_cast<double>(1u << kWeightBits) + 0.5);
    }
    return h;
}

std::map<std::string, std::string> parse_metadata(const std::string& line) {
    std::map<std::string, std::string> m;
    std::istringstream is(line.substr(1));
    std::string tok;
    while (is >> tok) {
        const std::size_t eq = tok.find('=');
        if (eq != std::string::npos) m[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    return m;
}

}  // namespace

std::string csv_metadata_line(const Options& opt) {
    std::string s = "#";
    for (const MetaField& f : current_metadata(opt)) s += " " + f.key + "=" + f.value;
    return s;
}

void append_csv_row(std::string& out, const ChainHit& h) {
    append_uint(out, h.seed);
    out += ',';
    append_uint(out, h.offset);
    out += ',';
    if (h.gap_count == 0) {
        out += '-';
    } else {
        for (std::size_t i = 0; i < h.gap_count; ++i) {
            if (i) out += '-';
            append_uint(out, h.gaps[i]);
        }
    }
    for (uint8_t e : h.elim) {
        out += ',';
        append_uint(out, e);
    }
    out += ',';
    append_uint(out, h.survivor);
    out += ',';
    if (h.volatility == 0.0) {
        out += '0';
    } else {
        char buf[32];
        const int n = std::snprintf(buf, sizeof buf, "%.4f", h.volatility);
        out.append(buf, static_cast<std::size_t>(n));
    }
    out += '\n';
}

CsvWriter::CsvWriter(const std::string& path, const Options& opt) : path_(path) {
    file_.open(path, std::ios::trunc | std::ios::binary);
    if (!file_) throw std::runtime_error("cannot open csv-out: " + path);
    file_ << csv_metadata_line(opt) << '\n'
          << "seed,offset,gap_path,e1,e2,e3,e4,e5,survivor,volatility\n";
    check_locked();
}

void CsvWriter::check_locked() {
    if (!file_.good()) throw std::runtime_error("csv-out write failed: " + path_);
}

void CsvWriter::write_block(const std::string& rows) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.write(rows.data(), static_cast<std::streamsize>(rows.size()));
    check_locked();
}

void CsvWriter::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open()) return;
    file_.flush();
    const bool ok = file_.good();
    file_.close();
    if (!ok || file_.fail()) throw std::runtime_error("csv-out write failed: " + path_);
}

std::vector<std::string> read_csv(const std::string& path, const Options& opt,
                                  const std::function<void(const ChainHit&)>& on_row) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open csv-in: " + path);

    std::vector<std::string> warnings;
    std::string raw;
    std::size_t line_no = 0;
    bool seen_row = false;
    bool seen_meta = false;
    while (std::getline(in, raw)) {
        ++line_no;
        const std::string line = strip_cr(raw);
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (!seen_meta && !seen_row) {
                seen_meta = true;
                const std::map<std::string, std::string> meta = parse_metadata(line);
                const bool fixed_domain_used =
                    opt.pick_rule != PickRule::DirectRemaining ||
                    (meta.count("pick") && meta.at("pick") != "direct");
                std::vector<std::string> keys{"model", "mapping", "pick", "offset_step", "rng_step"};
                if (fixed_domain_used) {
                    keys.insert(keys.end(), {"base_min", "base_max", "value_step"});
                }
                for (const MetaField& f : current_metadata(opt)) {
                    bool compared = false;
                    for (const std::string& k : keys) compared = compared || (k == f.key);
                    const auto it = meta.find(f.key);
                    if (compared && it != meta.end() && it->second != f.value) {
                        warnings.push_back("csv-in was produced with " + f.key + "=" + it->second +
                                           " but the current flags say " + f.key + "=" + f.value);
                    }
                }
            }
            continue;
        }
        if (!seen_row && line.rfind("seed,", 0) == 0) {
            seen_row = true;  // header
            continue;
        }
        seen_row = true;
        on_row(parse_row(line, path, line_no));
    }
    return warnings;
}

}  // namespace seedtracker
