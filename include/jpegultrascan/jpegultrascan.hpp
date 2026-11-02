#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <jpeglib.h>

namespace jpegultrascan {

enum class Engine {
    Libjpeg,  // A: encodes the perl's batch scripts with the linked libjpeg; the reference
    Direct,   // B: computes each scan's size from the coefficients
};

enum class DcScans {
    Upstream,     // jpegultrascan's default: one DC scan per component competes with one for all
    Interleaved,  // only one DC scan for all components, as jpegultrascan -i -1
};

struct Options {
    int max_bits = 3;       // most successive-approximation bits tried per band; capped at 10 for
                            // 8-bit images and 13 for 12-bit ones, as libjpeg does
    unsigned threads = 0;   // 0: hardware concurrency. the result never depends on it
    Engine engine = Engine::Libjpeg;
    DcScans dc = DcScans::Upstream;
};

struct Script {
    std::vector<jpeg_scan_info> scans;
    std::size_t bytes = 0;  // predicted size of the scans with their tables
};

// the scan script with the smallest total size for an image read with jpeg_read_coefficients;
// source and coefficients must stay alive and unchanged meanwhile. nothing on any error.
std::optional<Script> find_best_script(j_decompress_ptr source, jvirt_barray_ptr* coefficients,
                                       const Options& options);

enum class Markers {
    Keep,      // every APPn and COM segment, in order and as it is
    Jpegtran,  // what `jpegtran -copy all` writes, for comparing with jpegultrascan.pl
};

struct Result {
    std::vector<std::uint8_t> data;  // the re-encoded image, or a copy of the input if that is smaller
    Script script;
    bool improved = false;           // data is the re-encoded image
};

// reads one JPEG image, searches, encodes, and checks that the output decodes to the input's
// coefficients. bytes after the end of the image are not kept.
std::optional<Result> optimize(std::span<const std::uint8_t> jpeg, const Options& options,
                               Markers markers = Markers::Keep, std::string* error = nullptr);

}  // namespace jpegultrascan
