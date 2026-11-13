#include "seedtracker/compute/cpu_scanner.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "seedtracker/compute/chain.hpp"
#include "seedtracker/rng/draw_buffer.hpp"

namespace seedtracker {

namespace {

void worker(const Options& opt, CsvWriter* csv, std::atomic<uint64_t>& next_chunk, Stats& local) {
    ChainMatcher matcher(opt);
    DrawBuffer draws(effective_mapping(opt), matcher.window());
    std::string rows;
    if (csv) rows.reserve(kCsvFlushBytes + 4096);

    auto on_hit = [&](const ChainHit& h) {
        stats_add(local, h, opt);
        if (csv) append_csv_row(rows, h);
    };

    for (;;) {
        const uint64_t chunk = next_chunk.fetch_add(1, std::memory_order_relaxed);
        const uint64_t begin = opt.seed_begin + chunk * kSeedsPerChunk;
        if (begin >= opt.seed_end) break;
        const uint64_t end = std::min(opt.seed_end, begin + kSeedsPerChunk);
        for (uint64_t seed = begin; seed < end; ++seed) {
            draws.fill(static_cast<uint32_t>(seed));
            matcher.scan_seed(draws.data(), static_cast<uint32_t>(seed), on_hit);
            // FORMAT GUARANTEE, do not move this flush into on_hit or any
            // per-row path: flushing only between seeds makes all rows of one
            // (seed, offset, earlier gaps) group reach the file as one contiguous
            // run. filter_csv (apps/seedtracker/main.cpp) depends on this to
            // recompute volatility group by group while streaming.
            if (csv && rows.size() > kCsvFlushBytes) {
                csv->write_block(rows);
                rows.clear();
            }
        }
    }
    if (csv && !rows.empty()) csv->write_block(rows);
    local.invalid_events = matcher.invalid_events();
}

}  // namespace

Stats CpuScanner::scan(const Options& opt, CsvWriter* csv) {
    const uint64_t total_seeds = opt.seed_end - opt.seed_begin;
    const uint64_t chunks = (total_seeds + kSeedsPerChunk - 1) / kSeedsPerChunk;
    const unsigned workers = static_cast<unsigned>(std::min<uint64_t>(opt.threads, chunks));

    std::atomic<uint64_t> next_chunk{0};
    std::vector<Stats> locals(workers);
    for (Stats& s : locals) init_stats(s, opt);

    std::vector<std::thread> threads;
    std::exception_ptr failure;
    std::mutex failure_mutex;
    try {
        for (unsigned t = 0; t < workers; ++t) {
            threads.emplace_back([&, t]() {
                try {
                    worker(opt, csv, next_chunk, locals[t]);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                    next_chunk.store(chunks);  // stop the other workers
                }
            });
        }
    } catch (...) {
        // Creating a thread failed; joinable threads must not be destroyed.
        next_chunk.store(chunks);
        for (std::thread& th : threads) th.join();
        throw;
    }
    for (std::thread& th : threads) th.join();
    if (failure) std::rethrow_exception(failure);

    Stats all;
    init_stats(all, opt);
    for (const Stats& s : locals) stats_merge(all, s, opt);
    stats_finalize(all, opt);
    return all;
}

}  // namespace seedtracker
