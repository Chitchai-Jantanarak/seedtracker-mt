#include "seedtracker/logs/observation_log.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace seedtracker {

namespace {

bool parse_small_uint(const std::string& s, unsigned long long& out) {
    if (s.empty()) return false;
    unsigned long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
        if (v > (1ull << 40)) return false;
    }
    out = v;
    return true;
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    const std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

[[noreturn]] void fail(const std::string& name, std::size_t line, const std::string& msg) {
    throw std::runtime_error(name + ":" + std::to_string(line) + ": " + msg);
}

bool parse_gap_arg(const std::string& arg, unsigned long long& a, unsigned long long& b) {
    const std::size_t dots = arg.find("..");
    if (dots == std::string::npos) {
        if (!parse_small_uint(arg, a)) return false;
        b = a;
        return true;
    }
    return parse_small_uint(arg.substr(0, dots), a) && parse_small_uint(arg.substr(dots + 2), b);
}

}  // namespace

std::vector<int> parse_room_list(const std::string& s, const std::string& what, std::size_t max_len) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part = trim(part);
        if (part.empty()) continue;
        unsigned long long v = 0;
        if (!parse_small_uint(part, v) || v < 1 || v > 6) {
            throw std::runtime_error(what + " values must be integers 1..6, got '" + part + "'");
        }
        out.push_back(static_cast<int>(v));
    }
    if (out.size() > max_len) {
        throw std::runtime_error(what + " can contain at most " + std::to_string(max_len) + " values");
    }
    return out;
}

std::vector<Observation> parse_observation_text(const std::string& text, const std::string& name) {
    std::vector<Observation> obs;
    bool gap_pending = false;
    uint32_t gap_min = 0, gap_max = 0;
    std::size_t gap_line = 0;

    std::istringstream in(text);
    std::string raw;
    std::size_t line_no = 0;
    while (std::getline(in, raw)) {
        ++line_no;
        const std::size_t hash = raw.find('#');
        if (hash != std::string::npos) raw.erase(hash);
        const std::string line = trim(raw);
        if (line.empty()) continue;

        std::istringstream ls(line);
        std::string word;
        ls >> word;
        if (word == "gap") {
            if (obs.empty()) fail(name, line_no, "gap before the first event");
            if (gap_pending) fail(name, line_no, "two gap lines in a row");
            std::string arg, extra;
            ls >> arg;
            if (arg.empty()) fail(name, line_no, "gap needs a value: 'gap N' or 'gap A..B'");
            if (ls >> extra) fail(name, line_no, "unexpected text after gap value: '" + extra + "'");
            unsigned long long a = 0, b = 0;
            if (!parse_gap_arg(arg, a, b)) fail(name, line_no, "bad gap '" + arg + "' (expected N or A..B)");
            if (a > b) fail(name, line_no, "gap range start is greater than its end");
            if (b > kMaxGapValue) {
                fail(name, line_no, "gap above limit " + std::to_string(kMaxGapValue));
            }
            gap_pending = true;
            gap_min = static_cast<uint32_t>(a);
            gap_max = static_cast<uint32_t>(b);
            gap_line = line_no;
        } else if (word == "event") {
            if (obs.size() >= kMaxEvents) {
                fail(name, line_no, "more than " + std::to_string(kMaxEvents) + " events");
            }
            Observation o;
            std::string tok;
            bool first = true;
            while (ls >> tok) {
                const std::size_t eq = tok.find('=');
                try {
                    if (eq == std::string::npos) {
                        if (!first) throw std::runtime_error("unexpected token '" + tok + "'");
                        o.rule.prefix = parse_room_list(tok, "elimination list", 5);
                    } else {
                        const std::string key = tok.substr(0, eq);
                        const std::string val = tok.substr(eq + 1);
                        if (key == "survivor") {
                            const std::vector<int> v = parse_room_list(val, "survivor", 1);
                            if (v.size() != 1) throw std::runtime_error("survivor needs one room");
                            o.rule.survivor = v[0];
                        } else if (key == "exclude") {
                            for (int r : parse_room_list(val, "exclude", 5)) o.rule.exclude_mask |= 1u << r;
                        } else {
                            throw std::runtime_error("unknown key '" + key + "'");
                        }
                    }
                } catch (const std::exception& e) {
                    fail(name, line_no, e.what());
                }
                first = false;
            }
            if (!obs.empty()) {
                o.gap_min = gap_pending ? gap_min : 0;
                o.gap_max = gap_pending ? gap_max : 0;
            }
            gap_pending = false;
            obs.push_back(std::move(o));
        } else {
            fail(name, line_no, "unknown directive '" + word + "' (expected 'event' or 'gap')");
        }
    }
    if (gap_pending) fail(name, gap_line, "gap line has no event after it");
    if (obs.empty()) throw std::runtime_error(name + ": no events found");
    return obs;
}

std::vector<Observation> read_observation_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open obs file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_observation_text(ss.str(), path);
}

namespace {

std::string list_text(const std::vector<int>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += std::to_string(v[i]);
    }
    return s;
}

std::string exclude_text(unsigned mask) {
    std::string s;
    for (int r = 1; r <= 6; ++r) {
        if (mask & (1u << r)) {
            if (!s.empty()) s += ',';
            s += std::to_string(r);
        }
    }
    return s;
}

std::string gap_text(const Observation& o) {
    if (o.gap_min == o.gap_max) return std::to_string(o.gap_min);
    return std::to_string(o.gap_min) + ".." + std::to_string(o.gap_max);
}

}  // namespace

std::string format_observation_log(const std::vector<Observation>& obs) {
    std::string out;
    for (std::size_t i = 0; i < obs.size(); ++i) {
        if (i) out += "gap " + gap_text(obs[i]) + "\n";
        out += "event";
        if (!obs[i].rule.prefix.empty()) out += " " + list_text(obs[i].rule.prefix);
        if (obs[i].rule.survivor) out += " survivor=" + std::to_string(obs[i].rule.survivor);
        if (obs[i].rule.exclude_mask) out += " exclude=" + exclude_text(obs[i].rule.exclude_mask);
        out += "\n";
    }
    return out;
}

std::string summarize_observations(const std::vector<Observation>& obs) {
    if (obs.size() == 1 && obs[0].rule.survivor == 0 && obs[0].rule.exclude_mask == 0) {
        return obs[0].rule.prefix.empty() ? "(none)" : list_text(obs[0].rule.prefix);
    }
    std::string out;
    for (std::size_t i = 0; i < obs.size(); ++i) {
        if (i) out += "|g" + gap_text(obs[i]) + "|";
        out += obs[i].rule.prefix.empty() ? "-" : list_text(obs[i].rule.prefix);
        if (obs[i].rule.survivor) out += "+s" + std::to_string(obs[i].rule.survivor);
        if (obs[i].rule.exclude_mask) out += "+x" + exclude_text(obs[i].rule.exclude_mask);
    }
    return out;
}

}  // namespace seedtracker
