// Modified from jpegultrascan 1.4.0 (generatebitscans, generatedcscans, generateacscans and
// transcan), Copyright 2015-2024 Aaron Kaluszka, Apache-2.0.
// Changed: scripts are encoded in memory with the linked libjpeg instead of by jpegtran
// processes, and are not split at 60 scans (that limit came from jpegtran's script reader).
// Unless asked for the perl's exact batches, a band's chain of passes is cut after its last
// scan not measured yet, so each scan is encoded once instead of up to four times.

#include "engine_libjpeg.hpp"

#include <algorithm>
#include <unordered_set>

#include "parallel.hpp"
#include "scan_script.hpp"

namespace jpegultrascan::detail {

namespace {

using Scan = LibjpegEngine::Scan;
using Script = LibjpegEngine::Script;

std::string plane_text(const std::vector<int>& comps) {
    std::string s;
    for (std::size_t i = 0; i < comps.size(); ++i) s += (i ? " " : "") + std::to_string(comps[i]);
    return s;
}

Scan make_scan(const std::vector<int>& comps, int ss, int se, int ah, int al) {
    Scan s;
    s.info.comps_in_scan = static_cast<int>(comps.size());
    for (std::size_t i = 0; i < comps.size() && i < MAX_COMPS_IN_SCAN; ++i) s.info.component_index[i] = comps[i];
    s.info.Ss = ss;
    s.info.Se = se;
    s.info.Ah = ah;
    s.info.Al = al;
    s.text = plane_text(comps) + ": " + std::to_string(ss) + ' ' + std::to_string(se) + ' ' + std::to_string(ah) +
             ' ' + std::to_string(al) + ';';
    return s;
}

Scan make_sequential(const std::vector<int>& comps) {
    Scan s = make_scan(comps, 0, kMaxCoef, 0, 0);
    s.text = plane_text(comps) + ';';
    return s;
}

// a first pass at numbits, then one refinement per bit. with `measured`, only up to the last
// scan not in it: a refinement still needs the passes before it, the scans after it don't.
void add_bitscans(Script& script, const std::vector<int>& plane, int a, int b, int numbits,
                  std::unordered_set<std::string>* measured) {
    Script chain;
    chain.push_back(make_scan(plane, a, b, 0, numbits));
    for (int i = numbits; i > 0; --i) chain.push_back(make_scan(plane, a, b, i, i - 1));
    std::size_t keep = chain.size();
    if (measured) {
        while (keep > 0 && measured->count(chain[keep - 1].text)) --keep;
    }
    for (std::size_t i = 0; i < keep; ++i) {
        if (measured) measured->insert(chain[i].text);
        script.push_back(std::move(chain[i]));
    }
}

// "0 1 2: 0 0 0 9;": every AC scan needs a DC scan of its component in front of it. more
// than four components don't fit one scan (the perl would fail there), so they get several.
void add_dummies(Script& script, const std::vector<int>& comps) {
    for (std::size_t i = 0; i < comps.size(); i += MAX_COMPS_IN_SCAN) {
        const std::vector<int> group(comps.begin() + static_cast<std::ptrdiff_t>(i),
                                     comps.begin() + static_cast<std::ptrdiff_t>(std::min(comps.size(), i + MAX_COMPS_IN_SCAN)));
        script.push_back(make_scan(group, 0, 0, 0, 9));
    }
}

}  // namespace

LibjpegEngine::LibjpegEngine(const Source& source, int max_bits, unsigned threads, VerboseLog* log, bool perl_batches)
    : source_(source), max_bits_(max_bits), threads_(thread_count(threads)), log_(log),
      num_components_(source.info->num_components), perl_batches_(perl_batches) {}

bool LibjpegEngine::run(const std::vector<Script>& scripts,
                        const std::function<std::uint64_t*(const Scan&)>& slot) {
    std::vector<std::vector<std::size_t>> sizes(scripts.size());
    std::vector<std::string> errors(scripts.size());
    parallel_for(scripts.size(), threads_, [&](std::size_t k) {
        thread_local std::vector<std::uint8_t> out;
        std::vector<jpeg_scan_info> infos;
        infos.reserve(scripts[k].size());
        for (const Scan& s : scripts[k]) infos.push_back(s.info);
        std::string err;
        try {
            if (!encode(source_, infos, Markers::None, out, err)) {
                errors[k] = err.empty() ? "no data returned" : err;
                return;
            }
            sizes[k] = scan_sizes(out);
        } catch (...) {
            errors[k] = "out of memory";
            return;
        }
        if (sizes[k].size() != infos.size()) errors[k] = "incorrect encoding";
    });
    encodes_ += scripts.size();
    for (std::size_t k = 0; k < scripts.size(); ++k) {
        if (!errors[k].empty()) {
            error_ = errors[k] + " for:\n" + format_script([&] {
                std::vector<jpeg_scan_info> infos;
                for (const Scan& s : scripts[k]) infos.push_back(s.info);
                return infos;
            }());
            return false;
        }
        for (std::size_t i = 0; i < scripts[k].size(); ++i) {
            const std::uint64_t size = sizes[k][i];
            if (log_) log_->scan(scripts[k][i].text, size);
            if (std::uint64_t* s = slot(scripts[k][i])) {
                if (!*s) *s = size;
                else if (*s != size) ++inconsistencies_;
            }
        }
    }
    return true;
}

bool LibjpegEngine::measure_ac(int component, AcSizes& out) {
    std::vector<int> all(static_cast<std::size_t>(num_components_));
    for (int c = 0; c < num_components_; ++c) all[static_cast<std::size_t>(c)] = c;
    const std::vector<int> plane{component};
    std::unordered_set<std::string>* measured = perl_batches_ ? nullptr : &measured_;
    std::vector<Script> scripts;
    for (int w = 0; w < kMaxCoef; ++w) {
        const int offsets = std::min(w, kMaxCoef - 1 - w);
        for (int offset = 0; offset <= offsets; ++offset) {
            for (int numbits = 0; numbits <= max_bits_; ++numbits) {
                Script script;
                add_dummies(script, all);
                const std::size_t dummies = script.size();
                if (offset > 0) add_bitscans(script, plane, 1, offset, numbits, measured);
                int i = offset + 1;
                for (; i + w < kMaxCoef; i += w + 1) add_bitscans(script, plane, i, i + w, numbits, measured);
                if (i <= kMaxCoef) add_bitscans(script, plane, i, kMaxCoef, numbits, measured);
                if (script.size() > dummies) scripts.push_back(std::move(script));
            }
        }
    }
    return run(scripts, [&](const Scan& s) -> std::uint64_t* {
        if (s.info.Ss == 0 || s.info.comps_in_scan != 1 || s.info.component_index[0] != component) return nullptr;
        return &out.at(s.info.Ss, s.info.Se, s.info.Ah, s.info.Al);
    });
}

bool LibjpegEngine::measure_dc(const std::vector<int>& components, std::vector<std::uint64_t>& out) {
    std::vector<int> others;
    for (int c = 0; c < num_components_; ++c) {
        if (std::find(components.begin(), components.end(), c) == components.end()) others.push_back(c);
    }
    std::unordered_set<std::string>* measured = perl_batches_ ? nullptr : &measured_;
    std::vector<Script> scripts;
    for (int numbits = 0; numbits <= max_bits_; ++numbits) {
        Script script;
        add_bitscans(script, components, 0, 0, numbits, measured);
        if (script.empty()) continue;
        if (!others.empty()) add_dummies(script, others);
        scripts.push_back(std::move(script));
    }
    return run(scripts, [&](const Scan& s) -> std::uint64_t* {
        if (s.info.Se != 0 || s.info.comps_in_scan != static_cast<int>(components.size()) ||
            !std::equal(components.begin(), components.end(), s.info.component_index)) {
            return nullptr;
        }
        return &out[static_cast<std::size_t>(pass_index(max_bits_, s.info.Ah, s.info.Al))];
    });
}

bool LibjpegEngine::measure_sequential(const std::vector<std::vector<int>>& parts, std::vector<std::uint64_t>& out) {
    Script script;
    for (const auto& part : parts) script.push_back(make_sequential(part));
    out.assign(parts.size(), 0);
    std::size_t next = 0;
    std::vector<Script> scripts{script};
    return run(scripts, [&](const Scan&) -> std::uint64_t* { return &out[next++]; });
}

}  // namespace jpegultrascan::detail
