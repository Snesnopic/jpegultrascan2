#include "jpegultrascan/jpegultrascan.hpp"

#include <algorithm>

#include "engine_direct.hpp"
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

std::optional<Script> find_best_script(const Source& source, const Options& options, VerboseLog* log,
                                       std::string& error) {
    const SearchOptions search_options = search_options_for(*source.info, options, log);
    DirectEngine engine(source, search_options.max_bits, options.threads, log);
    if (!engine.ok()) {
        error = engine.error();
        return std::nullopt;
    }
    auto found = search(engine, source.info->num_components, search_options, error);
    if (!found) return std::nullopt;
    return Script{std::move(found->scans), static_cast<std::size_t>(found->bytes)};
}

}  // namespace detail

std::optional<Script> find_best_script(j_decompress_ptr source, jvirt_barray_ptr* coefficients,
                                       const Options& options) {
    try {
        detail::Source src{source, coefficients, {}};
        std::string error;
        if (!detail::collect_planes(src, error)) return std::nullopt;
        auto script = detail::find_best_script(src, options, nullptr, error);
        if (script && script->scans.size() > detail::kMaxScans) return std::nullopt;
        return script;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Result> optimize(std::span<const std::uint8_t> jpeg, const Options& options, Markers markers,
                               std::string* error) {
    std::string local;
    std::string& err = error ? *error : local;
    try {
        detail::Decoder source;
        if (!source.read(jpeg, true)) {
            err = source.error();
            return std::nullopt;
        }
        auto script = detail::find_best_script(source.source(), options, nullptr, err);
        if (!script) return std::nullopt;
        Result result;
        if (script->scans.size() > detail::kMaxScans) {
            result.data.assign(jpeg.begin(), jpeg.end());
            result.script = std::move(*script);
            return result;
        }
        const detail::Markers mode = markers == Markers::Keep ? detail::Markers::Keep : detail::Markers::Jpegtran;
        if (!detail::encode(source.source(), script->scans, mode, result.data, err)) return std::nullopt;
        detail::Decoder check;
        if (!check.read(result.data, true)) {
            err = "output doesn't decode: " + check.error();
            return std::nullopt;
        }
        if (!detail::same_image(source, check, mode == detail::Markers::Keep, err)) {
            err = "output differs from input: " + err;
            return std::nullopt;
        }
        result.script = std::move(*script);
        result.improved = result.data.size() <= jpeg.size();
        if (!result.improved) result.data.assign(jpeg.begin(), jpeg.end());
        return result;
    } catch (const std::exception& e) {
        err = e.what();
        return std::nullopt;
    }
}

}  // namespace jpegultrascan
