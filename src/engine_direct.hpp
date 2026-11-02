#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jpeg_io.hpp"
#include "search.hpp"

namespace jpegultrascan::detail {

// engine B: works out the bytes each scan would take the way libjpeg writes it (optimal
// Huffman tables, DHT and SOS segments, 0xFF stuffing) without encoding anything
class DirectEngine : public SizeSource {
public:
    // fails when libjpeg would refuse to encode the source; error() says why
    DirectEngine(const Source& source, int max_bits, unsigned threads, VerboseLog* log);

    bool ok() const { return error_.empty(); }
    bool measure_ac(int component, AcSizes& out) override;
    bool measure_dc(const std::vector<int>& components, std::vector<std::uint64_t>& out) override;
    bool measure_sequential(const std::vector<std::vector<int>>& parts, std::vector<std::uint64_t>& out) override;
    const char* error() const override { return error_.c_str(); }

    std::size_t scans() const { return scans_; }

    struct Job {
        jpeg_scan_info scan{};
        bool sequential = false;  // part of a sequential script: jchuff.c, not jcphuff.c
        std::uint64_t size = 0;
        bool failed = false;
    };
    // sizes any scans; AC scans of one component at a time, with Al <= max_bits
    bool measure(std::vector<Job>& jobs);

private:
    // per block, per Al up to max_bits: bit k set when |coefficient k in zigzag order| >> Al != 0
    struct Masks {
        int component = -1;
        std::vector<std::uint64_t> bits;
    };

    void size_of(Job& job) const;
    void build_masks(int component);

    const Source& source_;
    int max_bits_;
    unsigned threads_;
    VerboseLog* log_;
    int max_coef_bits_;
    std::vector<int> dc_tables_;
    std::vector<int> ac_tables_;
    Masks masks_;
    std::size_t scans_ = 0;
    std::string error_;
};

}  // namespace jpegultrascan::detail
