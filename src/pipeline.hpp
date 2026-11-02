#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "jpeg_io.hpp"
#include "jpegultrascan/jpegultrascan.hpp"
#include "search.hpp"

namespace jpegultrascan::detail {

struct Stats {
    std::size_t encodes = 0;          // libjpeg encodes engine A ran
    std::size_t inconsistencies = 0;  // scans engine A measured twice with different sizes
};

int max_bits_for(const jpeg_decompress_struct& info, int requested);

// perl_batches: engine A encodes exactly the perl's scripts (slower, same sizes)
std::optional<Script> find_best_script(const Source& source, const Options& options, VerboseLog* log,
                                       Stats* stats, std::string& error, bool perl_batches = false);

}  // namespace jpegultrascan::detail
