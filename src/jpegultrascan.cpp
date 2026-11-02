#include "jpegultrascan/jpegultrascan.hpp"

#include <algorithm>

#include "engine_direct.hpp"
#include "parallel.hpp"
#include "pipeline.hpp"

namespace jpegultrascan {

namespace detail {

int max_bits_for(const jpeg_decompress_struct& info, int requested) {
    // libjpeg's limit on Ah and Al
    return std::clamp(requested, 0, info.data_precision == 12 ? 13 : 10);
}

SearchOptions search_options_for(const jpeg_decompress_struct& info, const Options& options, VerboseLog* log) {
    SearchOptions search_options;
    search_options.max_bits = max_bits_for(info, options.max_bits);
    search_options.dc = options.dc == DcScans::Interleaved ? DcMode::Interleaved : DcMode::Upstream;
    search_options.log = log;
    return search_options;
}

ScriptResult find_best_script(const Source& source, const Options& options, VerboseLog* log) {
    ScriptResult result;
    try {
        const SearchOptions search_options = search_options_for(*source.info, options, log);
        DirectEngine engine(source, search_options.max_bits, loop_for(options), log);
        if (!engine.ok()) {
            result.status = Status::Unsupported;
            result.error = engine.error();
            return result;
        }
        auto found = search(engine, source.info->num_components, search_options, result.error);
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

}  // namespace detail

ScriptResult find_best_script(j_decompress_ptr source, jvirt_barray_ptr* coefficients, const Options& options) {
    ScriptResult result;
    try {
        detail::Source src{source, coefficients, {}};
        if (!detail::collect_planes(src, result.error)) return result;
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
        if (!source.read(jpeg, true)) {
            result.error = source.error();
            return result;
        }
        ScriptResult found = detail::find_best_script(source.source(), options, nullptr);
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
