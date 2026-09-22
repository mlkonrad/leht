// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The PDF side of signing: signature fields, the dictionary with its hole, the
// appearance, and reading signatures back. No cryptography here.
#include "leht/ops/sign.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace leht::ops {

namespace {

using OwnedAnnot = detail::Owned<pdf_annot, pdf_drop_annot>;
using OwnedList = detail::Owned<fz_display_list, fz_drop_display_list>;
using OwnedDevice = detail::Owned<fz_device, fz_drop_device>;
using OwnedFont = detail::Owned<fz_font, fz_drop_font>;

/// Owns a MuPDF object whose drop function takes a const pointer, which
/// detail::Owned cannot name.
template <typename T, auto Drop>
struct Holder {
    fz_context* ctx;
    T* ptr = nullptr;
    explicit Holder(fz_context* c) : ctx(c) {}
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
    ~Holder() {
        if (ptr != nullptr) {
            Drop(ctx, ptr);
        }
    }
};

/// Largest image accepted for an appearance, per side. A signature graphic is
/// small; a huge one is a decompression bomb, or a mistake.
constexpr int kMaxImageSide = 8000;
/// Longest hole: far above any real CMS blob with a timestamp and chain.
constexpr std::size_t kMaxReserve = std::size_t{1} << 20;
/// Largest /Contents read back for verification.
constexpr std::size_t kMaxContents = std::size_t{4} << 20;

// --- the placeholder signer --------------------------------------------------
//
// MuPDF completes a signature during the incremental save, asking its signer
// for the CMS blob. Ours answers with zeros: the blob is made later, by the
// process that holds the key. The struct is C layout, allocated by MuPDF.

struct HoleSigner {
    pdf_pkcs7_signer base;
    int refs;
    std::size_t size;
};

pdf_pkcs7_signer* hole_keep(fz_context* /*ctx*/, pdf_pkcs7_signer* s) {
    ++reinterpret_cast<HoleSigner*>(s)->refs;
    return s;
}

void hole_drop(fz_context* ctx, pdf_pkcs7_signer* s) {
    auto* h = reinterpret_cast<HoleSigner*>(s);
    if (--h->refs == 0) {
        fz_free(ctx, h);
    }
}

pdf_pkcs7_distinguished_name* hole_name(fz_context* ctx, pdf_pkcs7_signer* /*s*/) {
    auto* dn = static_cast<pdf_pkcs7_distinguished_name*>(
        fz_calloc(ctx, 1, sizeof(pdf_pkcs7_distinguished_name)));
    dn->cn = fz_strdup(ctx, "");
    return dn;
}

std::size_t hole_size(fz_context* /*ctx*/, pdf_pkcs7_signer* s) {
    return reinterpret_cast<HoleSigner*>(s)->size;
}

int hole_digest(fz_context* /*ctx*/, pdf_pkcs7_signer* /*s*/, fz_stream* /*in*/,
                unsigned char* digest, std::size_t len) {
    std::memset(digest, 0, len);
    return static_cast<int>(len);
}

void release_signer(fz_context* ctx, pdf_pkcs7_signer* s) { pdf_drop_signer(ctx, s); }

// --- the appearance -----------------------------------------------------------

fz_rect to_fz(const Rect& r) { return fz_make_rect(r.x0, r.y0, r.x1, r.y1); }

/// `inner` fitted into `box` with its aspect kept, centred.
fz_matrix fit(float w, float h, fz_rect box) {
    const float bw = box.x1 - box.x0;
    const float bh = box.y1 - box.y0;
    const float scale = std::min(bw / w, bh / h);
    const float x = box.x0 + (bw - w * scale) / 2;
    const float y = box.y0 + (bh - h * scale) / 2;
    return fz_concat(fz_scale(scale, scale), fz_translate(x, y));
}

void check_appearance(const Appearance& a) {
    for (const auto& stroke : a.strokes) {
        for (const Point& p : stroke) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                throw Error(0, "a drawn stroke has a point that is not a number");
            }
        }
    }
    if (!a.strokes.empty() && !(a.strokes_width > 0 && a.strokes_height > 0 &&
                                std::isfinite(a.strokes_width) && std::isfinite(a.strokes_height))) {
        throw Error(0, "drawn strokes need a canvas size");
    }
    if (!(a.stroke_width > 0 && std::isfinite(a.stroke_width))) {
        throw Error(0, "the stroke width must be positive");
    }
    for (const std::string& line : a.lines) {
        if (line.find('\0') != std::string::npos) {
            throw Error(0, "appearance text cannot contain NUL");
        }
    }
}

/// Draws `a` into a display list covering `rect` (page space).
OwnedList build_appearance(fz_context* c, fz_rect rect, const Appearance& a) {
    check_appearance(a);
    const bool graphic = !a.image.empty() || !a.strokes.empty();
    const bool text = !a.lines.empty();
    const float pad = std::min(3.0F, (rect.x1 - rect.x0) / 20);
    const fz_rect inner = fz_make_rect(rect.x0 + pad, rect.y0 + pad, rect.x1 - pad, rect.y1 - pad);
    fz_rect graphic_box = inner;
    fz_rect text_box = inner;
    if (graphic && text) {
        const float mid = (inner.x0 + inner.x1) / 2;
        graphic_box.x1 = mid;
        text_box.x0 = mid;
    }

    OwnedList list{c};
    OwnedDevice dev{c};
    guarded(c, [&](fz_context* g) {
        *list.slot() = fz_new_display_list(g, rect);
        *dev.slot() = fz_new_list_device(g, list.get());
    });

    if (!a.image.empty()) {
        detail::OwnedBuffer buffer{c};
        detail::OwnedImage image{c};
        const unsigned char* bytes = a.image.data();
        const std::size_t size = a.image.size();
        guarded(c, [&](fz_context* g) {
            *buffer.slot() = fz_new_buffer_from_copied_data(g, bytes, size);
            *image.slot() = fz_new_image_from_buffer(g, buffer.get());
        });
        const int w = image.get()->w;
        const int h = image.get()->h;
        if (w <= 0 || h <= 0 || w > kMaxImageSide || h > kMaxImageSide) {
            throw Error(0, "the signature image must be between 1 and " +
                               std::to_string(kMaxImageSide) + " pixels a side");
        }
        // An image is drawn into the unit square, so fit a 1x1 box scaled to
        // the image's aspect.
        const fz_matrix place = fit(static_cast<float>(w), static_cast<float>(h), graphic_box);
        const fz_matrix ctm = fz_pre_scale(place, static_cast<float>(w), static_cast<float>(h));
        guarded(c, [&](fz_context* g) {
            fz_fill_image(g, dev.get(), image.get(), ctm, 1.0F, fz_default_color_params);
        });
    }

    if (!a.strokes.empty()) {
        std::vector<fz_point> points;
        std::vector<int> counts;
        for (const auto& stroke : a.strokes) {
            for (const Point& p : stroke) {
                points.push_back(fz_make_point(p.x, p.y));
            }
            counts.push_back(static_cast<int>(stroke.size()));
        }
        const fz_matrix m = fit(a.strokes_width, a.strokes_height, graphic_box);
        const float width = a.stroke_width;
        const fz_point* pts = points.data();
        const int* cnt = counts.data();
        const int nstrokes = static_cast<int>(counts.size());
        Holder<fz_path, fz_drop_path> path{c};
        Holder<fz_stroke_state, fz_drop_stroke_state> stroke{c};
        guarded(c, [&](fz_context* g) {
            path.ptr = fz_new_path(g);
            const fz_point* p = pts;
            for (int s = 0; s < nstrokes; ++s) {
                for (int i = 0; i < cnt[s]; ++i, ++p) {
                    if (i == 0) {
                        fz_moveto(g, path.ptr, p->x, p->y);
                        if (cnt[s] == 1) {
                            fz_lineto(g, path.ptr, p->x, p->y);  // a dot
                        }
                    } else {
                        fz_lineto(g, path.ptr, p->x, p->y);
                    }
                }
            }
            stroke.ptr = fz_new_stroke_state(g);
            stroke.ptr->linewidth = width;
            stroke.ptr->start_cap = FZ_LINECAP_ROUND;
            stroke.ptr->end_cap = FZ_LINECAP_ROUND;
            stroke.ptr->dash_cap = FZ_LINECAP_ROUND;
            stroke.ptr->linejoin = FZ_LINEJOIN_ROUND;
            const float ink[3] = {0.05F, 0.1F, 0.35F};  // a pen's blue-black
            fz_stroke_path(g, dev.get(), path.ptr, stroke.ptr, m, fz_device_rgb(g), ink, 1.0F,
                           fz_default_color_params);
        });
    }

    if (text) {
        std::string joined;
        for (const std::string& line : a.lines) {
            if (!joined.empty()) {
                joined += '\n';
            }
            joined += line;
        }
        const char* str = joined.c_str();
        OwnedFont font{c};
        Holder<fz_text, fz_drop_text> laid{c};
        guarded(c, [&](fz_context* g) {
            *font.slot() = fz_new_base14_font(g, "Helvetica");
            laid.ptr = pdf_layout_fit_text(g, font.get(), FZ_LANG_UNSET, str, text_box);
            const float black[3] = {0, 0, 0};
            fz_fill_text(g, dev.get(), laid.ptr, fz_identity, fz_device_rgb(g), black, 1.0F,
                         fz_default_color_params);
        });
    }

    guarded(c, [&](fz_context* g) { fz_close_device(g, dev.get()); });
    return list;
}

std::string utc_stamp(std::int64_t t) {
    const auto tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tm);
    return buf;
}

// --- fields -------------------------------------------------------------------

/// A signature widget on some page, kept loaded.
struct FoundWidget {
    detail::OwnedPage page;
    OwnedAnnot widget;
    int index = -1;
};

std::string field_name(fz_context* c, pdf_obj* obj) {
    char* raw = nullptr;
    guarded(c, [&](fz_context* g) { raw = pdf_load_field_name(g, obj); });
    std::string out = raw != nullptr ? raw : "";
    fz_free(c, raw);
    return out;
}

/// The widget of the signature field named `name`, or throws.
FoundWidget find_signature_widget(fz_context* c, Document& doc, const std::string& name) {
    const int count = doc.page_count();
    for (int index = 0; index < count; ++index) {
        FoundWidget found{detail::load_pdf_page(c, doc, index), OwnedAnnot{c}, index};
        pdf_page* p = detail::as_pdf_page(c, found.page);
        std::vector<pdf_annot*> widgets;
        int n = 0;
        guarded(c, [&](fz_context* g) {
            for (pdf_annot* w = pdf_first_widget(g, p); w != nullptr; w = pdf_next_widget(g, w)) {
                ++n;
            }
        });
        widgets.assign(static_cast<std::size_t>(n), nullptr);
        pdf_annot** slots = widgets.data();
        guarded(c, [&](fz_context* g) {
            int i = 0;
            for (pdf_annot* w = pdf_first_widget(g, p); w != nullptr && i < n;
                 w = pdf_next_widget(g, w)) {
                slots[i++] = w;
            }
        });
        for (pdf_annot* w : widgets) {
            bool is_sig = false;
            pdf_obj* obj = nullptr;
            guarded(c, [&](fz_context* g) {
                obj = pdf_annot_obj(g, w);
                is_sig = pdf_name_eq(g, pdf_dict_get_inheritable(g, obj, PDF_NAME(FT)),
                                     PDF_NAME(Sig)) != 0;
            });
            if (!is_sig || field_name(c, obj) != name) {
                continue;
            }
            bool is_signed = false;
            bool read_only = false;
            guarded(c, [&](fz_context* g) {
                is_signed = pdf_widget_is_signed(g, w) != 0;
                read_only = pdf_widget_is_readonly(g, w) != 0;
                *found.widget.slot() = pdf_keep_annot(g, w);
            });
            if (is_signed) {
                throw Error(0, "the signature field \"" + name + "\" is already signed");
            }
            if (read_only) {
                throw Error(0, "the signature field \"" + name + "\" is read-only");
            }
            return found;
        }
    }
    throw Error(0, "no signature field named \"" + name + "\"");
}

/// A field name not in use: Signature1, Signature2, ...
std::string unused_name(fz_context* c, pdf_document* pdf) {
    for (int n = 1;; ++n) {
        const std::string name = "Signature" + std::to_string(n);
        const char* cname = name.c_str();
        bool taken = false;
        guarded(c, [&](fz_context* g) {
            pdf_obj* fields = pdf_dict_getp(g, pdf_trailer(g, pdf), "Root/AcroForm/Fields");
            taken = fields != nullptr && pdf_lookup_field(g, fields, cname) != nullptr;
        });
        if (!taken) {
            return name;
        }
    }
}

// --- reading signatures back ---------------------------------------------------

/// Signed signature fields, in field order. Bounded against cyclic /Kids.
void collect(fz_context* c, pdf_obj* field, pdf_obj* ft, int depth, int& budget,
             std::vector<pdf_obj*>& out) {
    if (depth > 32 || --budget < 0) {
        return;
    }
    pdf_obj* kids = nullptr;
    int k = 0;
    bool signed_sig = false;
    guarded(c, [&](fz_context* g) {
        pdf_obj* own = pdf_dict_get(g, field, PDF_NAME(FT));
        if (own != nullptr) {
            ft = own;
        }
        kids = pdf_dict_get(g, field, PDF_NAME(Kids));
        k = pdf_array_len(g, kids);
        pdf_obj* v = pdf_dict_get(g, field, PDF_NAME(V));
        signed_sig = pdf_name_eq(g, ft, PDF_NAME(Sig)) && pdf_is_dict(g, v);
    });
    if (signed_sig) {
        out.push_back(field);
        return;  // a signed field's kids are its widgets, not further fields
    }
    for (int i = 0; i < k; ++i) {
        pdf_obj* kid = nullptr;
        guarded(c, [&](fz_context* g) { kid = pdf_array_get(g, kids, i); });
        collect(c, kid, ft, depth + 1, budget, out);
    }
}

std::string text_of(fz_context* c, pdf_obj* dict, pdf_obj* key) {
    const char* s = nullptr;
    guarded(c, [&](fz_context* g) {
        pdf_obj* o = pdf_dict_get(g, dict, key);
        s = pdf_is_string(g, o) ? pdf_to_text_string(g, o) : nullptr;
    });
    return s != nullptr ? s : "";
}

std::string name_of(fz_context* c, pdf_obj* dict, pdf_obj* key) {
    const char* s = nullptr;
    guarded(c, [&](fz_context* g) {
        pdf_obj* o = pdf_dict_get(g, dict, key);
        s = pdf_is_name(g, o) ? pdf_to_name(g, o) : nullptr;
    });
    return s != nullptr ? s : "";
}

/// Reads `n` bytes at `offset` of the document's own file into `out`.
bool read_at(fz_context* c, pdf_document* pdf, std::int64_t offset, unsigned char* out,
             std::size_t n) {
    std::size_t got = 0;
    guarded(c, [&](fz_context* g) {
        fz_seek(g, pdf->file, offset, SEEK_SET);
        got = fz_read(g, pdf->file, out, n);
    });
    return got == n;
}

int hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/// The checks that make a ByteRange mean what it claims. See SignatureInfo.
void check_range(fz_context* c, pdf_document* pdf, pdf_obj* br, SignatureInfo& s) {
    bool four_ints = false;
    std::int64_t v[4] = {0, 0, 0, 0};
    std::int64_t file_size = 0;
    guarded(c, [&](fz_context* g) {
        file_size = pdf->file_size;
        four_ints = pdf_array_len(g, br) == 4;
        for (int i = 0; four_ints && i < 4; ++i) {
            pdf_obj* o = pdf_array_get(g, br, i);
            four_ints = pdf_is_int(g, o) != 0;
            v[i] = pdf_to_int64(g, o);
        }
    });
    if (!four_ints) {
        s.range_problem = "the byte range is not four integers (two spans)";
        return;
    }
    s.range.v = {v[0], v[1], v[2], v[3]};
    const ByteRange& r = s.range;
    if (v[0] != 0 || v[1] <= 0 || v[2] <= r.hole_begin() + 1 || v[3] < 0 || v[2] > file_size ||
        v[3] > file_size - v[2]) {
        s.range_problem = "the byte range does not lie within the file around one gap";
        return;
    }
    // The gap must be exactly the /Contents string: '<', hex, '>'. Otherwise a
    // signature could cover bytes that are not the ones this blob came from.
    const std::int64_t hole = r.hole_end() - r.hole_begin();
    if (hole < 2 || static_cast<std::size_t>(hole) > 2 * kMaxContents + 2) {
        s.range_problem = "the gap in the byte range is not a plausible signature";
        return;
    }
    std::vector<unsigned char> gap(static_cast<std::size_t>(hole));
    if (!read_at(c, pdf, r.hole_begin(), gap.data(), gap.size()) || gap.front() != '<' ||
        gap.back() != '>' || (gap.size() - 2) % 2 != 0) {
        s.range_problem = "the gap in the byte range is not the signature's hex string";
        return;
    }
    std::vector<std::uint8_t> decoded((gap.size() - 2) / 2);
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        const int hi = hex_value(gap[1 + 2 * i]);
        const int lo = hex_value(gap[2 + 2 * i]);
        if (hi < 0 || lo < 0) {
            s.range_problem = "the gap in the byte range is not the signature's hex string";
            return;
        }
        decoded[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    if (decoded != s.contents) {
        s.range_problem = "the gap in the byte range does not hold this signature's /Contents";
        return;
    }
    s.range_ok = true;
    s.changed_after_signing = r.end() < file_size;

    // Does the signed part end where a revision ends?
    const std::int64_t tail = std::min<std::int64_t>(64, r.end());
    std::vector<unsigned char> end(static_cast<std::size_t>(tail));
    if (read_at(c, pdf, r.end() - tail, end.data(), end.size())) {
        std::string t(end.begin(), end.end());
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) {
            t.pop_back();
        }
        s.covers_whole_revision = t.size() >= 5 && t.compare(t.size() - 5, 5, "%%EOF") == 0;
    }
}

}  // namespace

PreparedSignature prepare_signature(const Context& ctx, Document& doc,
                                    const SignatureRequest& request, int fd) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    if (!doc.can_save_incrementally()) {
        throw Error(0, doc.redacted()
                           ? "a redacted document must be saved in full before it is signed"
                           : "this document was repaired when opened; save it in full first, "
                             "then sign the saved file");
    }
    if (request.reserve < 1024 || request.reserve > kMaxReserve) {
        throw Error(0, "the signature reserve must be between 1 KB and 1 MB");
    }
    for (const std::string* s : {&request.name, &request.reason, &request.location, &request.field}) {
        if (s->find('\0') != std::string::npos) {
            throw Error(0, "signature fields cannot contain NUL");
        }
    }
    const std::int64_t when =
        request.time != 0 ? request.time : static_cast<std::int64_t>(std::time(nullptr));

    // The widget: an existing unsigned field, or a new one.
    FoundWidget target{detail::OwnedPage{c}, OwnedAnnot{c}, -1};
    if (!request.field.empty()) {
        target = find_signature_widget(c, doc, request.field);
    } else {
        if (request.page < 0 || request.page >= doc.page_count()) {
            throw Error(0, "page " + std::to_string(request.page + 1) + " is out of range");
        }
        for (float v : {request.rect.x0, request.rect.y0, request.rect.x1, request.rect.y1}) {
            if (!std::isfinite(v)) {
                throw Error(0, "the signature box is not a number");
            }
        }
        target.page = detail::load_pdf_page(c, doc, request.page);
        target.index = request.page;
        pdf_page* p = detail::as_pdf_page(c, target.page);
        std::string name = unused_name(c, pdf);
        char* cname = name.data();
        const fz_rect rect = request.rect.empty() ? fz_make_rect(0, 0, 0, 0) : to_fz(request.rect);
        guarded(c, [&](fz_context* g) {
            *target.widget.slot() = pdf_create_signature_widget(g, p, cname);
            pdf_obj* obj = pdf_annot_obj(g, target.widget.get());
            pdf_set_annot_rect(g, target.widget.get(), rect);
            // MuPDF gives a new field /Lock /All, which marks every other field
            // read-only and makes filling any of them later break the
            // signature. Signing a finished form is the common case, and a
            // lock is the form author's decision, not ours.
            pdf_dict_del(g, obj, PDF_NAME(Lock));
            if (fz_is_empty_rect(rect)) {
                // Invisible: printed-flag off, hidden and locked in place.
                pdf_dict_put_int(g, obj, PDF_NAME(F), PDF_ANNOT_IS_HIDDEN | PDF_ANNOT_IS_LOCKED);
            }
        });
    }

    fz_rect rect{};
    guarded(c, [&](fz_context* g) { rect = pdf_annot_rect(g, target.widget.get()); });
    const bool visible = !fz_is_empty_rect(rect);

    Appearance appearance = request.appearance;
    if (visible && appearance.lines.empty() && appearance.image.empty() &&
        appearance.strokes.empty()) {
        appearance.lines = {"Digitally signed by",
                            request.name.empty() ? std::string("(unnamed)") : request.name,
                            utc_stamp(when)};
    }
    OwnedList list{c};
    if (visible) {
        list = build_appearance(c, rect, appearance);
    }

    // The signer that leaves a hole.
    detail::Owned<pdf_pkcs7_signer, release_signer> signer{c};
    const std::size_t reserve = request.reserve;
    guarded(c, [&](fz_context* g) {
        auto* h = static_cast<HoleSigner*>(fz_calloc(g, 1, sizeof(HoleSigner)));
        h->base.keep = hole_keep;
        h->base.drop = hole_drop;
        h->base.get_signing_name = hole_name;
        h->base.max_digest_size = hole_size;
        h->base.create_digest = hole_digest;
        h->refs = 1;
        h->size = reserve;
        *signer.slot() = &h->base;
    });

    const char* sig_name = request.name.empty() ? nullptr : request.name.c_str();
    const char* reason = request.reason.empty() ? nullptr : request.reason.c_str();
    const char* location = request.location.empty() ? nullptr : request.location.c_str();
    fz_display_list* appearance_list = list.get();
    guarded(c, [&](fz_context* g) {
        pdf_obj* wobj = pdf_annot_obj(g, target.widget.get());
        pdf_dirty_annot(g, target.widget.get());
        if (appearance_list != nullptr) {
            pdf_set_annot_appearance_from_display_list(g, target.widget.get(), "N", nullptr,
                                                       fz_identity, appearance_list);
        }
        // The document now has signatures, and must only ever be appended to.
        pdf_obj* form = pdf_dict_getp(g, pdf_trailer(g, pdf), "Root/AcroForm");
        if (form == nullptr) {
            pdf_obj* root = pdf_dict_get(g, pdf_trailer(g, pdf), PDF_NAME(Root));
            form = pdf_dict_put_dict(g, root, PDF_NAME(AcroForm), 1);
        }
        const int flags = pdf_dict_get_int(g, form, PDF_NAME(SigFlags));
        pdf_dict_put_int(g, form, PDF_NAME(SigFlags), flags | 1 | 2);

        // The signature dictionary, written by hand rather than through
        // pdf_signature_set_value(), for two reasons. It hard-codes
        // /SubFilter /adbe.pkcs7.detached and adds a FieldMDP /Reference that
        // locks nothing; and once it has run, every further change to the
        // document opens a NEW incremental section, which would put the
        // signature's own revision before the end of the file and leave its
        // /ByteRange covering only part of it.
        //
        // /ByteRange, /Contents and /Filter must be written in that order and
        // close together: MuPDF finds them by name in the saved bytes to fill
        // the hole in. Every other key is added after them.
        pdf_obj* v = nullptr;
        const int num = pdf_create_object(g, pdf);
        pdf_dict_put_drop(g, wobj, PDF_NAME(V), pdf_new_indirect(g, pdf, num, 0));
        v = pdf_new_dict(g, pdf, 8);
        pdf_update_object(g, pdf, num, v);
        pdf_drop_obj(g, v);
        v = pdf_load_object(g, pdf, num);
        pdf_drop_obj(g, v);  // the xref keeps it

        pdf_dict_put_array(g, v, PDF_NAME(ByteRange), 4);
        auto* hole = static_cast<char*>(fz_calloc(g, reserve, 1));
        pdf_dict_put_string(g, v, PDF_NAME(Contents), hole, reserve);
        fz_free(g, hole);
        pdf_dict_put(g, v, PDF_NAME(Filter), PDF_NAME(Adobe_PPKLite));
        pdf_dict_put_name(g, v, PDF_NAME(SubFilter), "ETSI.CAdES.detached");
        pdf_dict_put(g, v, PDF_NAME(Type), PDF_NAME(Sig));
        pdf_dict_put_date(g, v, PDF_NAME(M), when);
        if (sig_name != nullptr) {
            pdf_dict_put_text_string(g, v, PDF_NAME(Name), sig_name);
        }
        if (reason != nullptr) {
            pdf_dict_put_text_string(g, v, PDF_NAME(Reason), reason);
        }
        if (location != nullptr) {
            pdf_dict_put_text_string(g, v, PDF_NAME(Location), location);
        }
        // From here MuPDF owns the completion: at save time it rewrites
        // /ByteRange with the real offsets and asks the signer for the blob.
        pdf_xref_store_unsaved_signature(g, pdf, wobj, signer.get());
    });

    pdf_obj* wobj = nullptr;
    guarded(c, [&](fz_context* g) { wobj = pdf_annot_obj(g, target.widget.get()); });
    PreparedSignature out;
    out.field = field_name(c, wobj);

    SaveOptions opts;
    opts.mode = SaveOptions::Mode::Incremental;
    doc.save_fd(fd, opts);

    // MuPDF rewrote /ByteRange with the real offsets while saving.
    std::int64_t r[4] = {0, 0, 0, 0};
    int len = 0;
    guarded(c, [&](fz_context* g) {
        pdf_obj* br = pdf_dict_getl(g, pdf_dict_get_inheritable(g, wobj, PDF_NAME(V)),
                                    PDF_NAME(ByteRange), nullptr);
        len = pdf_array_len(g, br);
        for (int i = 0; i < 4 && i < len; ++i) {
            r[i] = pdf_array_get_int(g, br, i);
        }
    });
    if (len != 4) {
        throw Error(0, "MuPDF did not write a two-span byte range");
    }
    out.range.v = {r[0], r[1], r[2], r[3]};
    return out;
}

std::vector<SignatureInfo> list_signatures(const Context& ctx, Document& doc) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);

    // Where each signature widget is: object number -> page and box.
    std::map<int, std::pair<int, Rect>> placed;
    const int count = doc.page_count();
    for (int index = 0; index < count; ++index) {
        detail::OwnedPage loaded = detail::load_pdf_page(c, doc, index);
        pdf_page* p = detail::as_pdf_page(c, loaded);
        int n = 0;
        guarded(c, [&](fz_context* g) {
            for (pdf_annot* w = pdf_first_widget(g, p); w != nullptr; w = pdf_next_widget(g, w)) {
                ++n;
            }
        });
        std::vector<int> nums(static_cast<std::size_t>(n), 0);
        std::vector<fz_rect> rects(static_cast<std::size_t>(n));
        int* num_slots = nums.data();
        fz_rect* rect_slots = rects.data();
        guarded(c, [&](fz_context* g) {
            int i = 0;
            for (pdf_annot* w = pdf_first_widget(g, p); w != nullptr && i < n;
                 w = pdf_next_widget(g, w), ++i) {
                num_slots[i] = pdf_to_num(g, pdf_annot_obj(g, w));
                rect_slots[i] = pdf_annot_rect(g, w);
            }
        });
        for (std::size_t i = 0; i < nums.size(); ++i) {
            placed.emplace(nums[i], std::make_pair(index, Rect{rects[i].x0, rects[i].y0,
                                                               rects[i].x1, rects[i].y1}));
        }
    }

    std::vector<pdf_obj*> fields;
    pdf_obj* top = nullptr;
    int k = 0;
    guarded(c, [&](fz_context* g) {
        top = pdf_dict_getp(g, pdf_trailer(g, pdf), "Root/AcroForm/Fields");
        k = pdf_array_len(g, top);
    });
    int budget = 100000;
    for (int i = 0; i < k; ++i) {
        pdf_obj* f = nullptr;
        guarded(c, [&](fz_context* g) { f = pdf_array_get(g, top, i); });
        collect(c, f, nullptr, 0, budget, fields);
    }

    std::vector<SignatureInfo> out;
    for (pdf_obj* field : fields) {
        SignatureInfo s;
        s.field = field_name(c, field);
        pdf_obj* v = nullptr;
        pdf_obj* widget = field;
        int num = 0;
        guarded(c, [&](fz_context* g) {
            v = pdf_dict_get(g, field, PDF_NAME(V));
            if (!pdf_name_eq(g, pdf_dict_get(g, field, PDF_NAME(Subtype)), PDF_NAME(Widget))) {
                pdf_obj* kid = pdf_array_get(g, pdf_dict_get(g, field, PDF_NAME(Kids)), 0);
                if (kid != nullptr) {
                    widget = kid;
                }
            }
            num = pdf_to_num(g, widget);
        });
        if (const auto it = placed.find(num); it != placed.end()) {
            s.page = it->second.first;
            s.rect = it->second.second;
        }
        s.filter = name_of(c, v, PDF_NAME(Filter));
        s.subfilter = name_of(c, v, PDF_NAME(SubFilter));
        s.name = text_of(c, v, PDF_NAME(Name));
        s.reason = text_of(c, v, PDF_NAME(Reason));
        s.location = text_of(c, v, PDF_NAME(Location));
        {
            const char* m = nullptr;
            guarded(c, [&](fz_context* g) {
                pdf_obj* o = pdf_dict_get(g, v, PDF_NAME(M));
                m = pdf_is_string(g, o) ? pdf_to_str_buf(g, o) : nullptr;
            });
            s.claimed_time = m != nullptr ? m : "";
        }
        const char* buf = nullptr;
        std::size_t len = 0;
        pdf_obj* br = nullptr;
        guarded(c, [&](fz_context* g) {
            pdf_obj* o = pdf_dict_get(g, v, PDF_NAME(Contents));
            if (pdf_is_string(g, o)) {
                buf = pdf_to_str_buf(g, o);
                len = pdf_to_str_len(g, o);
            }
            br = pdf_dict_get(g, v, PDF_NAME(ByteRange));
        });
        if (buf != nullptr && len <= kMaxContents) {
            s.contents.assign(reinterpret_cast<const std::uint8_t*>(buf),
                              reinterpret_cast<const std::uint8_t*>(buf) + len);
        }
        if (s.contents.empty()) {
            s.range_problem = "the signature has no /Contents";
        } else {
            check_range(c, pdf, br, s);
        }
        out.push_back(std::move(s));
    }
    // Which signatures' later bytes another signature signs as well.
    for (SignatureInfo& s : out) {
        if (!s.range_ok || !s.changed_after_signing) {
            continue;
        }
        for (const SignatureInfo& other : out) {
            if (other.range_ok && other.range.end() > s.range.end()) {
                s.later_signature_covers_changes = true;
                break;
            }
        }
    }
    return out;
}

ByteReader signed_bytes(const Context& ctx, const Document& doc, const ByteRange& range) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    struct State {
        int span = 0;
        std::int64_t done = 0;
    };
    auto state = std::make_shared<State>();
    return [c, pdf, range, state](std::uint8_t* buf, std::size_t size) -> std::size_t {
        while (state->span < 2) {
            const std::int64_t off = range.v[static_cast<std::size_t>(state->span * 2)];
            const std::int64_t len = range.v[static_cast<std::size_t>(state->span * 2 + 1)];
            if (state->done >= len) {
                ++state->span;
                state->done = 0;
                continue;
            }
            const auto want = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(size), len - state->done));
            std::size_t got = 0;
            const std::int64_t at = off + state->done;
            guarded(c, [&](fz_context* g) {
                fz_seek(g, pdf->file, at, SEEK_SET);
                got = fz_read(g, pdf->file, buf, want);
            });
            if (got == 0) {
                throw Error(0, "the signed bytes end before their byte range does");
            }
            state->done += static_cast<std::int64_t>(got);
            return got;
        }
        return 0;
    };
}

int add_signature_stamp(const Context& ctx, Document& doc, int page, const Rect& rect,
                        const Appearance& appearance) {
    fz_context* c = ctx.raw();
    (void)detail::require_pdf(c, doc);
    if (page < 0 || page >= doc.page_count()) {
        throw Error(0, "page " + std::to_string(page + 1) + " is out of range");
    }
    if (rect.empty()) {
        throw Error(0, "a signature stamp needs a box");
    }
    if (appearance.empty()) {
        throw Error(0, "a signature stamp needs an image, strokes or text");
    }
    const fz_rect box = to_fz(rect);
    OwnedList list = build_appearance(c, box, appearance);
    detail::OwnedPage loaded = detail::load_pdf_page(c, doc, page);
    pdf_page* p = detail::as_pdf_page(c, loaded);
    OwnedAnnot annot{c};
    int id = 0;
    guarded(c, [&](fz_context* g) {
        *annot.slot() = pdf_create_annot(g, p, PDF_ANNOT_STAMP);
        pdf_set_annot_rect(g, annot.get(), box);
        pdf_set_annot_icon_name(g, annot.get(), "LehtSignatureMark");
        pdf_set_annot_contents(g, annot.get(),
                               "Signature mark. This is a picture, not a digital signature: "
                               "it does not show who made it.");
        pdf_set_annot_appearance_from_display_list(g, annot.get(), "N", nullptr, fz_identity,
                                                   list.get());
        pdf_update_annot(g, annot.get());
        id = pdf_to_num(g, pdf_annot_obj(g, annot.get()));
    });
    return id;
}

}  // namespace leht::ops
