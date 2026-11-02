#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <jpeglib.h>

namespace jpegultrascan::detail {

// jpegtran's -scans syntax: "0 1 2: 0 0 0 9;" per scan, "0;" for a sequential scan, '#'
// starts a comment and other punctuation between numbers is ignored
std::optional<std::vector<jpeg_scan_info>> parse_scan_script(std::string_view text);

// one scan as jpegultrascan.pl writes it, without the trailing newline
std::string format_scan(const jpeg_scan_info& scan);
std::string format_script(std::span<const jpeg_scan_info> scans);

// the size scansizes() in jpegultrascan.pl gives each scan of an encoded file: the DHT
// segments in front of it, its SOS segment and its entropy-coded bytes
std::vector<std::size_t> scan_sizes(std::span<const std::uint8_t> file);

}  // namespace jpegultrascan::detail
