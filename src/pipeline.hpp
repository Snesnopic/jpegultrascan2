#pragma once

#include <optional>
#include <string>

#include "jpeg_io.hpp"
#include "jpegultrascan/jpegultrascan.hpp"
#include "search.hpp"

namespace jpegultrascan::detail {

int max_bits_for(const jpeg_decompress_struct& info, int requested);
SearchOptions search_options_for(const jpeg_decompress_struct& info, const Options& options, VerboseLog* log);

std::optional<Script> find_best_script(const Source& source, const Options& options, VerboseLog* log,
                                       std::string& error);

}  // namespace jpegultrascan::detail
