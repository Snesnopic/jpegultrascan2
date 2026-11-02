#include "jpegultrascan/jpegultrascan.hpp"

#include <algorithm>

#include "engine_direct.hpp"
#include "engine_libjpeg.hpp"
#include "pipeline.hpp"

namespace jpegultrascan {

namespace detail {

int max_bits_for(const jpeg_decompress_struct& info, int requested) {
    // libjpeg's limit on Ah and Al
    return std::clamp(requested, 0, info.data_precision == 12 ? 13 : 10);
}

std::optional<Script> find_best_script(const Source& source, const Options& options, VerboseLog* log,
                                       Stats* stats, std::string& error, bool perl_batches) {
    const jpeg_decompress_struct& info = *source.info;
    SearchOptions search_options;
    search_options.max_bits = max_bits_for(info, options.max_bits);
    search_options.dc = options.dc == DcScans::Interleaved ? DcMode::Interleaved : DcMode::Upstream;
    search_options.log = log;
    if (options.engine == Engine::Direct) {
        DirectEngine engine(source, search_options.max_bits, options.threads, log);
        if (!engine.ok()) {
            error = engine.error();
            return std::nullopt;
        }
        auto found = search(engine, info.num_components, search_options, error);
        if (!found) return std::nullopt;
        return Script{std::move(found->scans), static_cast<std::size_t>(found->bytes)};
    }
    LibjpegEngine engine(source, search_options.max_bits, options.threads, log, perl_batches);
    auto found = search(engine, info.num_components, search_options, error);
    if (stats) {
        stats->encodes = engine.encodes();
        stats->inconsistencies = engine.inconsistencies();
    }
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
        return detail::find_best_script(src, options, nullptr, nullptr, error);
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
        auto script = detail::find_best_script(source.source(), options, nullptr, nullptr, err);
        if (!script) return std::nullopt;
        const detail::Markers mode = markers == Markers::Keep ? detail::Markers::Keep : detail::Markers::Jpegtran;
        Result result;
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
