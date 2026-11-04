#include "jpegultrascan/jpegultrascan.hpp"

#include <algorithm>
#include <map>

#include "engine_direct.hpp"
#include "parallel.hpp"
#include "pipeline.hpp"

namespace jpegultrascan {

namespace detail {

int max_bits_for(const jpeg_decompress_struct& info, int requested) {
    // libjpeg's limit on Ah and Al
    return std::clamp(requested, 0, info.data_precision == 12 ? kMostBits : 10);
}

SearchOptions search_options_for(const jpeg_decompress_struct& info, const Options& options, VerboseLog* log) {
    SearchOptions search_options;
    search_options.max_bits = max_bits_for(info, options.max_bits);
    search_options.dc = options.dc == DcScans::Interleaved ? DcMode::Interleaved : DcMode::Upstream;
    search_options.log = log;
    return search_options;
}

namespace {

// passes on what the engine measures and keeps it, so that the search can run again with fewer
// bits without measuring anything: a scan's size doesn't depend on max_bits
class Kept : public SizeSource {
public:
    Kept(SizeSource& source, int max_bits) : source_(source), max_bits_(max_bits), bits_(max_bits) {}

    // from now on, answer from what was kept as a search with at most `bits` asks
    void limit(int bits) { bits_ = bits; }

    bool measure_ac(int component, AcSizes& out) override {
        if (bits_ == max_bits_) {
            if (!source_.measure_ac(component, out)) return false;
            ac_.insert_or_assign(component, out);
            return true;
        }
        const AcSizes& all = ac_.at(component);
        for (int ss = 1; ss <= kMaxCoef; ++ss) {
            for (int se = ss; se <= kMaxCoef; ++se) {
                for (int al = 0; al <= bits_; ++al) out.at(ss, se, 0, al) = all.get(ss, se, 0, al);
                for (int ah = 1; ah <= bits_; ++ah) out.at(ss, se, ah, ah - 1) = all.get(ss, se, ah, ah - 1);
            }
        }
        return true;
    }

    bool measure_dc(const std::vector<int>& components, std::vector<std::uint64_t>& out) override {
        if (bits_ == max_bits_) {
            if (!source_.measure_dc(components, out)) return false;
            dc_.insert_or_assign(components, out);
            return true;
        }
        const std::vector<std::uint64_t>& all = dc_.at(components);
        const auto at = [](int bits, int ah, int al) { return static_cast<std::size_t>(pass_index(bits, ah, al)); };
        for (int al = 0; al <= bits_; ++al) out[at(bits_, 0, al)] = all[at(max_bits_, 0, al)];
        for (int ah = 1; ah <= bits_; ++ah) out[at(bits_, ah, ah - 1)] = all[at(max_bits_, ah, ah - 1)];
        return true;
    }

    bool measure_sequential(const std::vector<std::vector<int>>& parts, std::vector<std::uint64_t>& out) override {
        if (bits_ == max_bits_) {
            if (!source_.measure_sequential(parts, out)) return false;
            sequential_.insert_or_assign(parts, out);
            return true;
        }
        out = sequential_.at(parts);
        return true;
    }

    const char* error() const override { return source_.error(); }

private:
    SizeSource& source_;
    int max_bits_;
    int bits_;
    std::map<int, AcSizes> ac_;
    std::map<std::vector<int>, std::vector<std::uint64_t>> dc_;
    std::map<std::vector<std::vector<int>>, std::vector<std::uint64_t>> sequential_;
};

}  // namespace

ScriptResult find_best_script(const Source& source, const Options& options, VerboseLog* log, std::size_t max_scans) {
    ScriptResult result;
    try {
        const SearchOptions search_options = search_options_for(*source.info, options, log);
        DirectEngine engine(source, search_options.max_bits, loop_for(options), log);
        if (!engine.ok()) {
            result.status = Status::Unsupported;
            result.error = engine.error();
            return result;
        }
        Kept kept(engine, search_options.max_bits);
        const int n = source.info->num_components;
        auto found = search(kept, n, search_options, result.error);
        for (int bits = search_options.max_bits - 1; found && found->scans.size() > max_scans && bits >= 0; --bits) {
            kept.limit(bits);
            SearchOptions fewer = search_options;
            fewer.max_bits = bits;
            fewer.log = nullptr;
            found = search(kept, n, fewer, result.error);
        }
        if (!found) {
            // the engine fails on what libjpeg wouldn't encode
            result.status = engine.ok() ? Status::Error : Status::Unsupported;
            return result;
        }
        result.script = {std::move(found->scans), static_cast<std::size_t>(found->bytes)};
        result.status = Status::Optimized;
    } catch (const Stopped&) {
        result = {};
        result.status = Status::Interrupted;
    } catch (const std::exception& e) {
        result = {};
        result.error = e.what();
    }
    return result;
}

namespace {

// past memory_limit with `extra` bytes besides what the search takes
bool over_memory(const Source& source, const Options& options, std::size_t extra, ScriptResult& result) {
    if (!options.memory_limit) return false;
    const unsigned pieces = options.parallel_for ? options.threads : thread_count(options.threads);
    const std::size_t engine = DirectEngine::memory_for(source, max_bits_for(*source.info, options.max_bits), pieces);
    if (engine + extra <= options.memory_limit) return false;
    result.status = Status::Unsupported;
    result.error = "image too large for the memory limit";
    return true;
}

}  // namespace

}  // namespace detail

ScriptResult find_best_script(j_decompress_ptr source, jvirt_barray_ptr* coefficients, const Options& options) {
    ScriptResult result;
    try {
        detail::Source src{source, coefficients, {}};
        if (!detail::collect_planes(src, result.error)) return result;
        if (detail::over_memory(src, options, 0, result)) return result;
        result = detail::find_best_script(src, options, nullptr);
    } catch (const std::exception& e) {
        result = {};
        result.error = e.what();
        return result;
    }
    if (result.status == Status::Optimized && result.script.scans.size() > detail::kMaxScans) {
        result.status = Status::Unsupported;
        result.error = "mozjpeg can't encode more than " + std::to_string(detail::kMaxScans) + " scans";
    }
    return result;
}

Result optimize(std::span<const std::uint8_t> jpeg, const Options& options, Markers markers) {
    Result result;
    try {
        detail::Decoder source;
        if (!source.read(jpeg, true, options.memory_limit)) {
            if (source.over_limit()) result.status = Status::Unsupported;
            result.error = source.error();
            return result;
        }
        // besides the search: the input's coefficients, the output (which may grow to twice its
        // size), and the coefficients again to check it
        const std::size_t besides = 2 * detail::coefficient_bytes(*source.info()) + 2 * jpeg.size();
        ScriptResult found;
        if (!detail::over_memory(source.source(), options, besides, found)) {
            found = detail::find_best_script(source.source(), options, nullptr);
        }
        result.status = found.status;
        result.script = std::move(found.script);
        result.error = std::move(found.error);
        if (result.status != Status::Optimized) return result;
        if (result.script.scans.size() > detail::kMaxScans) {
            result.status = Status::Unsupported;
            result.error = "mozjpeg can't encode more than " + std::to_string(detail::kMaxScans) + " scans";
            return result;
        }
        result.status = Status::Error;
        const detail::Markers mode = markers == Markers::Keep ? detail::Markers::Keep : detail::Markers::Jpegtran;
        if (!detail::encode(source.source(), result.script.scans, mode, result.data, result.error)) {
            result.data.clear();
            return result;
        }
        detail::Decoder check;
        if (!check.read(result.data, true)) {
            result.error = "output doesn't decode: " + check.error();
            result.data.clear();
            return result;
        }
        if (!detail::same_image(source, check, mode == detail::Markers::Keep, result.error)) {
            result.error = "output differs from input: " + result.error;
            result.data.clear();
            return result;
        }
        result.status = result.data.size() < jpeg.size() ? Status::Optimized : Status::NotSmaller;
        if (result.status == Status::NotSmaller) result.data.clear();
    } catch (const std::exception& e) {
        result = {};
        result.error = e.what();
    }
    return result;
}

}  // namespace jpegultrascan
