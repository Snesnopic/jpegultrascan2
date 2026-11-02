#include "jpeg_io.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

#include <jerror.h>

namespace jpegultrascan::detail {

namespace {

void error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    std::longjmp(err->jump, 1);
}

// a library doesn't print: the last warning stays in the manager
void output_message(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
}

void source_init(j_decompress_ptr) {}

// past the end of the data: warn and insert an EOI, as jpeg_mem_src and jpeg_stdio_src do
boolean source_fill(j_decompress_ptr cinfo) {
    static const JOCTET eoi[2] = {0xFF, JPEG_EOI};
    reinterpret_cast<MemorySource*>(cinfo->src)->hit_end = true;
    WARNMS(cinfo, JWRN_JPEG_EOF);
    cinfo->src->next_input_byte = eoi;
    cinfo->src->bytes_in_buffer = 2;
    return TRUE;
}

void source_skip(j_decompress_ptr cinfo, long num_bytes) {
    jpeg_source_mgr* src = cinfo->src;
    if (num_bytes <= 0) return;
    while (num_bytes > static_cast<long>(src->bytes_in_buffer)) {
        num_bytes -= static_cast<long>(src->bytes_in_buffer);
        (void)(*src->fill_input_buffer)(cinfo);
    }
    src->next_input_byte += num_bytes;
    src->bytes_in_buffer -= static_cast<std::size_t>(num_bytes);
}

void source_term(j_decompress_ptr) {}

struct VectorDestination {
    jpeg_destination_mgr pub{};
    std::vector<std::uint8_t>* out = nullptr;
};

// a failed allocation leaves no room, so the next write asks to grow and fails inside libjpeg
void destination_init(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestination*>(cinfo->dest);
    try {
        dest->out->resize(std::max<std::size_t>(dest->out->capacity(), 1 << 16));
    } catch (...) {
        dest->out->clear();
    }
    dest->pub.next_output_byte = dest->out->data();
    dest->pub.free_in_buffer = dest->out->size();
}

// returning FALSE while compressing makes libjpeg fail with JERR_CANT_SUSPEND
boolean destination_grow(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestination*>(cinfo->dest);
    const std::size_t used = dest->out->size();
    try {
        dest->out->resize(used ? used * 2 : 1 << 16);
    } catch (...) {
        return FALSE;
    }
    dest->pub.next_output_byte = dest->out->data() + used;
    dest->pub.free_in_buffer = dest->out->size() - used;
    return TRUE;
}

void destination_term(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestination*>(cinfo->dest);
    dest->out->resize(dest->out->size() - dest->pub.free_in_buffer);
}

bool is_segment(const jpeg_marker_struct& m, int marker, const char* id) {
    const std::size_t n = std::strlen(id) + 1;
    return m.marker == marker && m.data_length >= n && std::memcmp(m.data, id, n) == 0;
}

struct Encoder {
    jpeg_compress_struct cinfo{};
    ErrorManager err{};
    VectorDestination dest{};
    bool created = false;
    Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    ~Encoder() {
        if (created) jpeg_destroy_compress(&cinfo);
    }
};

const JQUANT_TBL* quant_table(const jpeg_decompress_struct& cinfo, int ci) {
    const jpeg_component_info& c = cinfo.comp_info[ci];
    if (c.quant_table) return c.quant_table;
    if (c.quant_tbl_no < 0 || c.quant_tbl_no >= NUM_QUANT_TBLS) return nullptr;
    return cinfo.quant_tbl_ptrs[c.quant_tbl_no];
}

}  // namespace

void init_error_manager(ErrorManager& err) {
    jpeg_std_error(&err.pub);
    err.pub.error_exit = error_exit;
    err.pub.output_message = output_message;
}

std::size_t coefficient_bytes(const jpeg_decompress_struct& cinfo) {
    std::size_t total = 0;
    for (int ci = 0; ci < cinfo.num_components; ++ci) {
        const jpeg_component_info& c = cinfo.comp_info[ci];
        const std::size_t h = static_cast<std::size_t>(c.h_samp_factor);
        const std::size_t v = static_cast<std::size_t>(c.v_samp_factor);
        const std::size_t w = (c.width_in_blocks + h - 1) / h * h;
        const std::size_t rows = (c.height_in_blocks + v - 1) / v * v;
        total += w * rows * sizeof(JBLOCK);
    }
    return total;
}

Decoder::~Decoder() {
    if (created_) jpeg_destroy_decompress(&cinfo_);
}

bool Decoder::read(std::span<const std::uint8_t> data, bool save_markers, std::size_t memory_limit) {
    if (created_) {
        error_ = "decoder already used";
        return false;
    }
    init_error_manager(err_);
    cinfo_.err = &err_.pub;
    src_.pub.init_source = source_init;
    src_.pub.fill_input_buffer = source_fill;
    src_.pub.skip_input_data = source_skip;
    src_.pub.resync_to_restart = jpeg_resync_to_restart;
    src_.pub.term_source = source_term;
    src_.pub.next_input_byte = data.data();
    src_.pub.bytes_in_buffer = data.size();

    if (setjmp(err_.jump)) {
        error_ = err_.message;
        source_ = {};
        return false;
    }
    jpeg_create_decompress(&cinfo_);
    created_ = true;
    cinfo_.src = &src_.pub;
    if (save_markers) {
        jpeg_save_markers(&cinfo_, JPEG_COM, 0xFFFF);
        for (int m = 0; m < 16; ++m) jpeg_save_markers(&cinfo_, JPEG_APP0 + m, 0xFFFF);
    }
    if (memory_limit) {
        cinfo_.mem->max_memory_to_use = static_cast<long>(std::min<std::size_t>(memory_limit, LONG_MAX));
    }
    if (jpeg_read_header(&cinfo_, TRUE) != JPEG_HEADER_OK) {
        error_ = "no image";
        return false;
    }
    if (memory_limit && coefficient_bytes(cinfo_) > memory_limit) {
        error_ = "image too large for the memory limit";
        return false;
    }
    jvirt_barray_ptr* coefficients = jpeg_read_coefficients(&cinfo_);
    if (!coefficients) {
        error_ = "no coefficients";
        return false;
    }
    source_.info = &cinfo_;
    source_.coefficients = coefficients;
    if (!collect_planes(source_, error_)) {
        source_ = {};
        return false;
    }
    return true;
}

namespace {

struct ErrorSwap {
    j_decompress_ptr info;
    jpeg_error_mgr* saved;
    ~ErrorSwap() { info->err = saved; }
};

bool collect_planes_guarded(Source& source, ErrorManager& err, std::string& error) {
    j_decompress_ptr cinfo = source.info;
    if (setjmp(err.jump)) {
        error = err.message;
        return false;
    }
    // the arrays live in memory (libjpeg-turbo and mozjpeg have no backing store), so the
    // row pointers stay valid as long as the decompressor
    for (int ci = 0; ci < cinfo->num_components; ++ci) {
        Plane& p = source.planes[static_cast<std::size_t>(ci)];
        for (JDIMENSION r = 0; r < p.height_in_blocks; ++r) {
            p.rows[r] = (*cinfo->mem->access_virt_barray)(reinterpret_cast<j_common_ptr>(cinfo),
                                                          source.coefficients[ci], r, 1, FALSE)[0];
        }
    }
    return true;
}

}  // namespace

bool collect_planes(Source& source, std::string& error) {
    j_decompress_ptr cinfo = source.info;
    if (!cinfo || !source.coefficients || cinfo->num_components < 1) {
        error = "no coefficients";
        return false;
    }
    source.planes.assign(static_cast<std::size_t>(cinfo->num_components), Plane{});
    for (int ci = 0; ci < cinfo->num_components; ++ci) {
        Plane& p = source.planes[static_cast<std::size_t>(ci)];
        p.width_in_blocks = cinfo->comp_info[ci].width_in_blocks;
        p.height_in_blocks = cinfo->comp_info[ci].height_in_blocks;
        p.rows.assign(p.height_in_blocks, nullptr);
    }
    ErrorManager err;
    init_error_manager(err);
    const ErrorSwap swap{cinfo, cinfo->err};
    cinfo->err = &err.pub;
    return collect_planes_guarded(source, err, error);
}

std::size_t Decoder::trailing_bytes() const {
    if (!created_ || src_.hit_end || !cinfo_.src) return 0;
    return cinfo_.src->bytes_in_buffer;
}

bool encode(const Source& source, std::span<const jpeg_scan_info> scans, Markers markers,
            std::vector<std::uint8_t>& out, std::string& error) {
    Encoder enc;
    init_error_manager(enc.err);
    enc.cinfo.err = &enc.err.pub;
    enc.dest.out = &out;
    enc.dest.pub.init_destination = destination_init;
    enc.dest.pub.empty_output_buffer = destination_grow;
    enc.dest.pub.term_destination = destination_term;
    j_decompress_ptr src = source.info;

    if (setjmp(enc.err.jump)) {
        error = enc.err.message;
        out.clear();
        return false;
    }
    jpeg_create_compress(&enc.cinfo);
    enc.created = true;
#ifdef JPEGULTRASCAN_MOZJPEG
    // mozjpeg's default profile runs its own scan search and progression
    jpeg_c_set_int_param(&enc.cinfo, JINT_COMPRESS_PROFILE, JCP_FASTEST);
#endif
    jpeg_copy_critical_parameters(src, &enc.cinfo);
    if (markers == Markers::Jpegtran) {
        // jtransform_adjust_parameters: some decoders choke on other sampling factors
        if (enc.cinfo.num_components == 1) {
            enc.cinfo.comp_info[0].h_samp_factor = 1;
            enc.cinfo.comp_info[0].v_samp_factor = 1;
        }
        // and no JFIF segment in front of an Exif one that comes first
        const jpeg_marker_struct* first = src->marker_list;
        if (first && first->marker == JPEG_APP0 + 1 && first->data_length >= 6 &&
            std::memcmp(first->data, "Exif\0\0", 6) == 0) {
            enc.cinfo.write_JFIF_header = FALSE;
        }
    }
    if (markers == Markers::Keep) {
        // the source's own segments are copied with the rest
        enc.cinfo.write_JFIF_header = FALSE;
        enc.cinfo.write_Adobe_marker = FALSE;
    }
    enc.cinfo.optimize_coding = TRUE;
    if (!scans.empty()) {
        enc.cinfo.scan_info = scans.data();
        enc.cinfo.num_scans = static_cast<int>(scans.size());
    }
    enc.cinfo.dest = &enc.dest.pub;
    jpeg_write_coefficients(&enc.cinfo, source.coefficients);
    if (markers != Markers::None) {
        for (jpeg_saved_marker_ptr m = src->marker_list; m; m = m->next) {
            if (markers == Markers::Jpegtran) {
                // jcopy_markers_execute: no second JFIF or Adobe segment
                if (enc.cinfo.write_JFIF_header && is_segment(*m, JPEG_APP0, "JFIF")) continue;
                if (enc.cinfo.write_Adobe_marker && m->marker == JPEG_APP0 + 14 && m->data_length >= 5 &&
                    std::memcmp(m->data, "Adobe", 5) == 0)
                    continue;
            }
            jpeg_write_marker(&enc.cinfo, m->marker, m->data, m->data_length);
        }
    }
    jpeg_finish_compress(&enc.cinfo);
    return true;
}

bool huffman_tables(const Source& source, std::vector<int>& dc, std::vector<int>& ac, std::string& error) {
    Encoder enc;
    init_error_manager(enc.err);
    enc.cinfo.err = &enc.err.pub;
    if (setjmp(enc.err.jump)) {
        error = enc.err.message;
        return false;
    }
    jpeg_create_compress(&enc.cinfo);
    enc.created = true;
#ifdef JPEGULTRASCAN_MOZJPEG
    jpeg_c_set_int_param(&enc.cinfo, JINT_COMPRESS_PROFILE, JCP_FASTEST);
#endif
    // jpeg_set_colorspace picks them from the color space
    jpeg_copy_critical_parameters(source.info, &enc.cinfo);
    dc.clear();
    ac.clear();
    for (int ci = 0; ci < enc.cinfo.num_components; ++ci) {
        dc.push_back(enc.cinfo.comp_info[ci].dc_tbl_no);
        ac.push_back(enc.cinfo.comp_info[ci].ac_tbl_no);
    }
    return true;
}

bool same_image(const Decoder& a, const Decoder& b, bool same_markers, std::string& why) {
    const jpeg_decompress_struct& x = a.info();
    const jpeg_decompress_struct& y = b.info();
    if (x.image_width != y.image_width || x.image_height != y.image_height) {
        why = "dimensions differ";
        return false;
    }
    if (x.num_components != y.num_components || x.data_precision != y.data_precision) {
        why = "components or precision differ";
        return false;
    }
    if (x.jpeg_color_space != y.jpeg_color_space) {
        why = "color space differs";
        return false;
    }
    if (a.planes().size() != static_cast<std::size_t>(x.num_components) ||
        b.planes().size() != static_cast<std::size_t>(y.num_components)) {
        why = "coefficients missing";
        return false;
    }
    for (int ci = 0; ci < x.num_components; ++ci) {
        const jpeg_component_info& cx = x.comp_info[ci];
        const jpeg_component_info& cy = y.comp_info[ci];
        if (cx.component_id != cy.component_id) {
            why = "component ids differ";
            return false;
        }
        // jpegtran writes 1x1 for a single component, which doesn't change its blocks
        if (x.num_components > 1 &&
            (cx.h_samp_factor != cy.h_samp_factor || cx.v_samp_factor != cy.v_samp_factor)) {
            why = "sampling factors differ";
            return false;
        }
        const JQUANT_TBL* qx = quant_table(x, ci);
        const JQUANT_TBL* qy = quant_table(y, ci);
        if (!qx || !qy || std::memcmp(qx->quantval, qy->quantval, sizeof(qx->quantval)) != 0) {
            why = "quantization tables differ";
            return false;
        }
        const Plane& px = a.planes()[static_cast<std::size_t>(ci)];
        const Plane& py = b.planes()[static_cast<std::size_t>(ci)];
        if (px.width_in_blocks != py.width_in_blocks || px.height_in_blocks != py.height_in_blocks) {
            why = "block counts differ";
            return false;
        }
        for (unsigned r = 0; r < px.height_in_blocks; ++r) {
            if (std::memcmp(px.rows[r], py.rows[r], sizeof(JBLOCK) * px.width_in_blocks) != 0) {
                why = "coefficients differ in component " + std::to_string(ci) + ", block row " + std::to_string(r);
                return false;
            }
        }
    }
    if (same_markers) {
        const jpeg_marker_struct* mx = x.marker_list;
        const jpeg_marker_struct* my = y.marker_list;
        for (; mx && my; mx = mx->next, my = my->next) {
            if (mx->marker != my->marker || mx->data_length != my->data_length ||
                std::memcmp(mx->data, my->data, mx->data_length) != 0) {
                why = "segments differ";
                return false;
            }
        }
        if (mx || my) {
            why = "segment counts differ";
            return false;
        }
    }
    return true;
}

}  // namespace jpegultrascan::detail
