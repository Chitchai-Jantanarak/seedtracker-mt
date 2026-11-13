// seedtracker: offline MT19937 candidate scanner for a Sakray/rAthena-style RNG chain.
//
//   std::mt19937 -> rnd() -> pick rule -> elimination model
//
// Flow: parse flags -> validate (warnings) -> pick backend -> scan (or re-filter a CSV) -> report.
//
// "offset" means the number of SERVER rnd() calls before the event, not raw
// engine calls. This tool never touches a server; it only simulates candidate
// seeds and offsets offline.
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>

#include "seedtracker/compute/backend_select.hpp"
#include "seedtracker/compute/chain.hpp"
#include "seedtracker/config/cli.hpp"
#include "seedtracker/export/csv.hpp"
#include "seedtracker/export/report.hpp"
#include "seedtracker/logs/observation_log.hpp"
#include "seedtracker/rng/mapping_probe.hpp"

namespace {

using namespace seedtracker;

// True when both rows belong to the same (seed, offset, earlier gaps) group,
// i.e. they differ at most in the last gap. Single-event rows are their own group.
bool same_group(const ChainHit& a, const ChainHit& b) {
    if (a.gap_count == 0 || a.seed != b.seed || a.offset != b.offset || a.gap_count != b.gap_count) return false;
    for (std::size_t i = 0; i + 1 < a.gap_count; ++i) {
        if (a.gaps[i] != b.gaps[i]) return false;
    }
    return true;
}

// Re-filters a saved candidate CSV by the predicted event's post-rule.
//
// A scan writes all rows of one group contiguously (see CpuScanner), so the
// filter buffers one group at a time, keeps the rows that pass the rule, and
// recomputes volatility over the survivors for the current prefix and depth.
Stats filter_csv(const Options& opt) {
    std::unique_ptr<CsvWriter> out;
    if (!opt.csv_out.empty()) out = std::make_unique<CsvWriter>(opt.csv_out, opt);

    Stats s;
    init_stats(s, opt);
    std::string rows;
    const PostRule& rule = predicted_rule(opt);
    const std::size_t prefix_len = predicted_prefix_len(opt);

    std::vector<ChainHit> group;
    std::vector<uint32_t> keys;
    auto flush_group = [&]() {
        keys.clear();
        std::vector<ChainHit*> kept;
        for (ChainHit& h : group) {
            if (!rule_matches(rule, h.elim.data())) continue;
            kept.push_back(&h);
            keys.push_back(prediction_key(h.elim.data(), prefix_len, opt.depth));
        }
        if (!kept.empty() && kept.front()->gap_count > 0) {
            const VolatilityScore score = score_predictions(keys);
            for (ChainHit* h : kept) {
                h->volatility = score.volatility;
                h->weight_q = score.weight_q;
            }
        }
        for (ChainHit* h : kept) {
            stats_add(s, *h, opt);
            if (out) append_csv_row(rows, *h);
        }
        if (out && rows.size() > kCsvFlushBytes) {
            out->write_block(rows);
            rows.clear();
        }
        group.clear();
    };

    const std::vector<std::string> warnings = read_csv(opt.csv_in, opt, [&](const ChainHit& h) {
        if (!group.empty() && !same_group(group.back(), h)) flush_group();
        group.push_back(h);
    });
    flush_group();
    for (const std::string& w : warnings) std::cerr << "warning: " << w << "\n";
    if (out) {
        out->write_block(rows);
        out->close();
    }
    stats_finalize(s, opt);
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);
        if (opt.help) {
            usage(std::cout, argv[0]);
            return 0;
        }
        if (opt.probe_mapping) {
            std::cout << probe_std_mapping().report;
            return 0;
        }
        for (const std::string& w : validate(opt)) std::cerr << "warning: " << w << "\n";

        if (opt.emit_events) {
            std::cout << format_observation_log(simulate_chain(opt, opt.emit_seed, opt.emit_offset, opt.emit_gaps));
            return 0;
        }

        Stats stats;
        if (opt.csv_in.empty()) {
            std::unique_ptr<CsvWriter> csv;
            if (!opt.csv_out.empty()) csv = std::make_unique<CsvWriter>(opt.csv_out, opt);
            BackendSelection sel = select_backend(opt);
            for (const std::string& w : sel.warnings) std::cerr << "warning: " << w << "\n";
            if (!sel.note.empty()) std::cerr << sel.note << "\n";
            const auto t0 = std::chrono::steady_clock::now();
            stats = sel.backend->scan(opt, csv.get());
            if (csv) csv->close();
            const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
            std::cerr << "scan time: " << dt.count() << " s (" << sel.description << ")\n";
        } else {
            stats = filter_csv(opt);
        }
        print_stats(std::cout, stats, opt);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n(run with --help for usage)\n";
        return 1;
    }
}
