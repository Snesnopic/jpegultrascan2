#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "jpeg_io.hpp"
#include "search.hpp"

namespace jpegultrascan::detail {

// engine A, the reference: encodes the same batch scripts jpegultrascan.pl hands to jpegtran,
// in memory with the linked libjpeg, and reads each scan's size the way scansizes does
class LibjpegEngine : public SizeSource {
public:
    // perl_batches: encode exactly the perl's scripts, measuring most scans several times
    LibjpegEngine(const Source& source, int max_bits, unsigned threads, VerboseLog* log, bool perl_batches = false);

    bool measure_ac(int component, AcSizes& out) override;
    bool measure_dc(const std::vector<int>& components, std::vector<std::uint64_t>& out) override;
    bool measure_sequential(const std::vector<std::vector<int>>& parts, std::vector<std::uint64_t>& out) override;
    const char* error() const override { return error_.c_str(); }

    // scans measured more than once with different sizes: 0 unless a scan's size depends on
    // the rest of the script, which the whole search assumes it doesn't
    std::size_t inconsistencies() const { return inconsistencies_; }
    std::size_t encodes() const { return encodes_; }

    struct Scan {
        jpeg_scan_info info{};
        std::string text;
    };
    using Script = std::vector<Scan>;

private:
    // measures the scripts in parallel, then logs and stores their sizes in script order;
    // slot(scan) is where a scan's size goes, null for the dummy DC scans
    bool run(const std::vector<Script>& scripts,
             const std::function<std::uint64_t*(const Scan&)>& slot);

    const Source& source_;
    int max_bits_;
    unsigned threads_;
    VerboseLog* log_;
    int num_components_;
    bool perl_batches_;
    std::unordered_set<std::string> measured_;
    std::size_t inconsistencies_ = 0;
    std::size_t encodes_ = 0;
    std::string error_;
};

}  // namespace jpegultrascan::detail
