#pragma once

#include "jpeg_io.hpp"
#include "jpegultrascan/jpegultrascan.hpp"
#include "search.hpp"

namespace jpegultrascan::detail {

int max_bits_for(const jpeg_decompress_struct& info, int requested);
SearchOptions search_options_for(const jpeg_decompress_struct& info, const Options& options, VerboseLog* log);

// Optimized with the script, Unsupported, Interrupted or Error. a script beyond max_scans is
// searched again with fewer bits, on the same sizes, until one fits; it may still not fit
ScriptResult find_best_script(const Source& source, const Options& options, VerboseLog* log,
                              std::size_t max_scans = kMaxScans);

}  // namespace jpegultrascan::detail
