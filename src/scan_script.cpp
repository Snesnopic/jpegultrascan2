// Modified from jpegultrascan 1.4.0 (scansizes), Copyright 2015-2024 Aaron Kaluszka, Apache-2.0.

#include "scan_script.hpp"

#include <cctype>

namespace jpegultrascan::detail {

std::optional<std::vector<jpeg_scan_info>> parse_scan_script(std::string_view text) {
    std::vector<jpeg_scan_info> scans;
    std::vector<int> numbers;
    bool colon = false;
    int before_colon = 0;
    const auto finish = [&]() -> bool {
        if (numbers.empty() && !colon) return true;
        jpeg_scan_info s{};
        const int ncomps = colon ? before_colon : static_cast<int>(numbers.size());
        if (ncomps < 1 || ncomps > MAX_COMPS_IN_SCAN) return false;
        if (colon && static_cast<int>(numbers.size()) != ncomps + 4) return false;
        s.comps_in_scan = ncomps;
        for (int i = 0; i < ncomps; ++i) s.component_index[i] = numbers[static_cast<std::size_t>(i)];
        if (colon) {
            s.Ss = numbers[static_cast<std::size_t>(ncomps)];
            s.Se = numbers[static_cast<std::size_t>(ncomps) + 1];
            s.Ah = numbers[static_cast<std::size_t>(ncomps) + 2];
            s.Al = numbers[static_cast<std::size_t>(ncomps) + 3];
        } else {
            s.Ss = 0;
            s.Se = DCTSIZE2 - 1;
            s.Ah = 0;
            s.Al = 0;
        }
        scans.push_back(s);
        numbers.clear();
        colon = false;
        return true;
    };
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '#') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            long v = 0;
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
                v = v * 10 + (text[i] - '0');
                if (v > 1000) return std::nullopt;
                ++i;
            }
            numbers.push_back(static_cast<int>(v));
        } else if (c == ':') {
            if (colon) return std::nullopt;
            colon = true;
            before_colon = static_cast<int>(numbers.size());
            ++i;
        } else if (c == ';') {
            if (!finish()) return std::nullopt;
            ++i;
        } else {
            ++i;
        }
    }
    if (!finish()) return std::nullopt;
    if (scans.empty()) return std::nullopt;
    return scans;
}

std::string format_scan(const jpeg_scan_info& scan) {
    std::string s;
    for (int i = 0; i < scan.comps_in_scan; ++i) {
        if (i) s += ' ';
        s += std::to_string(scan.component_index[i]);
    }
    if (scan.Ss == 0 && scan.Se == DCTSIZE2 - 1 && scan.Ah == 0 && scan.Al == 0) return s + ';';
    return s + ": " + std::to_string(scan.Ss) + ' ' + std::to_string(scan.Se) + ' ' + std::to_string(scan.Ah) +
           ' ' + std::to_string(scan.Al) + ';';
}

std::string format_script(std::span<const jpeg_scan_info> scans) {
    std::string s;
    for (const jpeg_scan_info& scan : scans) {
        if (!s.empty()) s += '\n';
        s += format_scan(scan);
    }
    return s;
}

// same walk as the perl: every run of 0xFF followed by a nonzero byte is a marker, whose
// segment is skipped by its length; SOF opens the count, SOS starts entropy-coded data
// that runs to the next marker, and each scan also takes the segments before its SOS
std::vector<std::size_t> scan_sizes(std::span<const std::uint8_t> file) {
    std::vector<std::size_t> sizes;
    std::size_t scanstart = 0;
    std::size_t nextoff = 0;
    const std::size_t n = file.size();
    std::size_t pos = 0;
    while (pos < n) {
        // find /\xFF+(.)/ from pos
        std::size_t start = pos;
        while (start < n && file[start] != 0xFF) ++start;
        if (start >= n) break;
        std::size_t end = start;
        while (end < n && file[end] == 0xFF) ++end;
        if (end >= n) {
            // the regex backtracks: the last 0xFF becomes the captured byte
            if (end - start < 2) break;
            --end;
        }
        const std::size_t match_end = end + 1;  // past the captured byte
        pos = match_end;
        if (match_end < nextoff + 2) continue;
        const unsigned type = file[end];
        if (!type) continue;
        const std::size_t tagstart = start < nextoff ? nextoff : start;
        std::size_t length = 0;
        if (type != 0xD8 && type != 0xD9) {
            // unpack 'n' past the end of the data gives 0 in perl
            const unsigned hi = match_end < n ? file[match_end] : 0;
            const unsigned lo = match_end + 1 < n ? file[match_end + 1] : 0;
            length = match_end + 1 < n ? (hi << 8 | lo) : 0;
        }
        const std::size_t len = length + match_end - tagstart;
        nextoff = tagstart + len;
        if (scanstart) {
            sizes.back() += tagstart - scanstart;
            if (type == 0xD9) break;
            sizes.push_back(len);
            scanstart = 0;
        } else if (sizes.empty() && type >= 0xC0 && type <= 0xCF && type != 0xC4 && type != 0xC8 && type != 0xCC) {
            sizes.push_back(0);
        } else if (!sizes.empty()) {
            sizes.back() += len;
        }
        if (type == 0xDA) scanstart = tagstart + len;
    }
    return sizes;
}

}  // namespace jpegultrascan::detail
