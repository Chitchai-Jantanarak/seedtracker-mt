#pragma once
// CSV export and re-import of surviving candidates.
//
// File layout:
//   # model=remaining mapping=std pick=direct base_min=1 base_max=10000 value_step=1 offset_step=1 rng_step=1 obs=5,1
//   seed,offset,gap_path,e1,e2,e3,e4,e5,survivor,volatility
//   123456,3,17-40,2,4,1,6,3,5,0
//
// gap_path is "-" for a single-event run, otherwise the chosen gaps joined by
// '-'. e1..e5 and survivor describe the last (predicted) event. Reading also
// accepts the legacy 8-column layout (seed,offset,e1..e5,survivor).
#include <cstddef>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "seedtracker/config/options.hpp"
#include "seedtracker/types.hpp"

namespace seedtracker {

// The "# key=value ..." line describing how a CSV was produced.
std::string csv_metadata_line(const Options& opt);

// Appends one CSV row (with trailing newline) to `out`.
void append_csv_row(std::string& out, const ChainHit& h);

// Row buffers are flushed to the file once they pass this size.
constexpr std::size_t kCsvFlushBytes = std::size_t{1} << 20;

// Opens the file, writes the metadata line and header. write_block() may be
// called from several threads; each call writes its rows as one contiguous run.
// Write errors throw std::runtime_error("csv-out write failed: <path>"). Call
// close() when done so a failed final flush is reported instead of swallowed by
// the destructor.
class CsvWriter {
public:
    CsvWriter(const std::string& path, const Options& opt);
    void write_block(const std::string& rows);
    void close();

private:
    void check_locked();

    std::string path_;
    std::ofstream file_;
    std::mutex mutex_;
};

// Reads `path`, validates every row (rooms 1..6, seed <= UINT32_MAX, errors name
// the line), and calls `on_row` for each. Skips '#' lines. Returns warnings,
// e.g. when the file's metadata differs from the current flags.
std::vector<std::string> read_csv(const std::string& path, const Options& opt,
                                  const std::function<void(const ChainHit&)>& on_row);

}  // namespace seedtracker
