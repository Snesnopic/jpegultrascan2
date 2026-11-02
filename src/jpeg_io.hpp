#pragma once

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include <jpeglib.h>

namespace jpegultrascan::detail {

// libjpeg's default error_exit calls exit(): errors jump back here instead. code between a
// setjmp and the libjpeg call that may jump must not own objects with destructors.
struct ErrorManager {
    jpeg_error_mgr pub{};
    std::jmp_buf jump{};
    char message[JMSG_LENGTH_MAX]{};
};

void init_error_manager(ErrorManager& err);

struct MemorySource {
    jpeg_source_mgr pub{};
    bool hit_end = false;
};

// one component's coefficient blocks, borrowed from the decoder's virtual array
struct Plane {
    unsigned width_in_blocks = 0;
    unsigned height_in_blocks = 0;
    std::vector<JBLOCKROW> rows;
};

// an image read down to its DCT coefficients, by a Decoder or by the caller
struct Source {
    j_decompress_ptr info = nullptr;
    jvirt_barray_ptr* coefficients = nullptr;
    std::vector<Plane> planes;
};

// fills source.planes from the virtual arrays. libjpeg reports errors through the source's
// own error manager, which may be the caller's, so ours stands in for it meanwhile.
bool collect_planes(Source& source, std::string& error);

// reads one JPEG image held in memory down to its DCT coefficients
class Decoder {
public:
    Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;
    ~Decoder();

    // data must outlive the decoder; memory_limit 0 means no limit
    bool read(std::span<const std::uint8_t> data, bool save_markers, std::size_t memory_limit = 0);

    j_decompress_ptr info() { return &cinfo_; }
    const jpeg_decompress_struct& info() const { return cinfo_; }
    jvirt_barray_ptr* coefficients() const { return source_.coefficients; }
    const std::vector<Plane>& planes() const { return source_.planes; }
    const Source& source() const { return source_; }
    int warnings() const { return static_cast<int>(err_.pub.num_warnings); }
    // the first warning, or the error
    const char* last_message() const { return err_.message; }
    const std::string& error() const { return error_; }
    // bytes after the end of the image that libjpeg didn't read
    std::size_t trailing_bytes() const;

private:
    jpeg_decompress_struct cinfo_{};
    ErrorManager err_{};
    MemorySource src_{};
    bool created_ = false;
    Source source_;
    std::string error_;
};

// libjpeg's estimate of the memory the coefficients of an image take
std::size_t coefficient_bytes(const jpeg_decompress_struct& cinfo);

enum class Markers {
    None,      // `jpegtran -copy none`: only the JFIF or Adobe segment libjpeg writes itself
    Keep,      // every saved APPn and COM segment, in order and as it is
    Jpegtran,  // `jpegtran -copy all`: its own JFIF or Adobe segment replaces the source's
};

// mozjpeg 4.1.5 records each scan's Al in an array of 64 (actual_Al in jcmaster.h) even when
// it doesn't search scans itself, so a longer script overruns it
#ifdef JPEGULTRASCAN_MOZJPEG
inline constexpr std::size_t kMaxScans = 64;
#else
inline constexpr std::size_t kMaxScans = SIZE_MAX;
#endif

// encodes the decoder's coefficients with a scan script (sequential when empty) and
// optimized Huffman tables, as jpegtran -optimize -scans does; fails beyond kMaxScans scans
bool encode(const Source& source, std::span<const jpeg_scan_info> scans, Markers markers,
            std::vector<std::uint8_t>& out, std::string& error);

// the Huffman table numbers libjpeg gives each component when it encodes the source
bool huffman_tables(const Source& source, std::vector<int>& dc, std::vector<int>& ac, std::string& error);

// true when b holds the same image as a: geometry, color space, quantization tables and
// every DCT coefficient; with same_markers also the same APPn and COM segments
bool same_image(const Decoder& a, const Decoder& b, bool same_markers, std::string& why);

}  // namespace jpegultrascan::detail
