// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Document Security Store (/DSS, PAdES B-LT): the certificates, OCSP
// responses and CRLs a validator needs to check a signature long after its
// certificates expire, or after their revocation services are gone.
//
// Reading it takes whatever is there, /VRI included, within limits: the
// document is untrusted. Writing it appends a revision that touches only the
// catalog's /DSS entry, which is why every signature stays intact and a
// certification allows it at every level.
#include "leht/ops/sign.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <set>
#include <string>
#include <vector>

namespace leht::ops {

namespace {

using Blob = std::vector<std::uint8_t>;

/// Limits on what is read from a document's /DSS.
constexpr int kMaxItems = 4096;
constexpr std::size_t kMaxItemBytes = std::size_t{16} << 20;  // a large CRL
constexpr std::size_t kMaxTotalBytes = std::size_t{128} << 20;

struct Reader {
    fz_context* c;
    pdf_document* pdf;
    ValidationData* out;
    int items = 0;
    std::size_t bytes = 0;
    std::set<int> seen;  ///< stream objects already read (VRI repeats the lists)

    /// Reads one stream, decoded, into `to`.
    void stream(pdf_obj* ref, std::vector<Blob>& to) {
        if (items >= kMaxItems || bytes >= kMaxTotalBytes) {
            return;
        }
        bool is_stream = false;
        int num = 0;
        guarded(c, [&](fz_context* g) {
            is_stream = pdf_is_stream(g, ref) != 0;
            num = pdf_to_num(g, ref);
        });
        if (!is_stream || (num != 0 && !seen.insert(num).second)) {
            return;
        }
        detail::OwnedBuffer buf{c};
        try {
            guarded(c, [&](fz_context* g) { *buf.slot() = pdf_load_stream(g, ref); });
        } catch (const Error&) {
            return;  // a broken stream is skipped, not fatal
        }
        unsigned char* data = nullptr;
        std::size_t len = 0;
        guarded(c, [&](fz_context* g) { len = fz_buffer_storage(g, buf.get(), &data); });
        if (len == 0 || len > kMaxItemBytes || bytes + len > kMaxTotalBytes) {
            return;
        }
        to.emplace_back(data, data + len);
        bytes += len;
        ++items;
    }

    /// Every stream of the array (or single stream) at `key` in `dict`.
    void list(pdf_obj* dict, const char* key, std::vector<Blob>& to) {
        pdf_obj* v = nullptr;
        int n = 0;
        bool array = false;
        guarded(c, [&](fz_context* g) {
            v = pdf_dict_gets(g, dict, key);
            array = pdf_is_array(g, v) != 0;
            n = array ? pdf_array_len(g, v) : 0;
        });
        if (!array) {
            if (v != nullptr) {
                stream(v, to);
            }
            return;
        }
        for (int i = 0; i < n && items < kMaxItems; ++i) {
            pdf_obj* item = nullptr;
            guarded(c, [&](fz_context* g) { item = pdf_array_get(g, v, i); });
            stream(item, to);
        }
    }
};

/// The array at `key` in `dss` to append to, created when missing.
pdf_obj* array_for(fz_context* g, pdf_document* pdf, pdf_obj* dss, const char* key) {
    if (!pdf_is_array(g, pdf_dict_gets(g, dss, key))) {
        pdf_dict_puts_drop(g, dss, key, pdf_new_array(g, pdf, 8));
    }
    return pdf_dict_gets(g, dss, key);
}

}  // namespace

ValidationData read_dss(const Context& ctx, Document& doc) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    ValidationData out;
    pdf_obj* dss = nullptr;
    guarded(c, [&](fz_context* g) { dss = pdf_dict_getp(g, pdf_trailer(g, pdf), "Root/DSS"); });
    if (dss == nullptr) {
        return out;
    }
    Reader r{c, pdf, &out, 0, 0, {}};
    r.list(dss, "Certs", out.certs);
    r.list(dss, "OCSPs", out.ocsps);
    r.list(dss, "CRLs", out.crls);
    // /VRI: per-signature copies, keyed by the signature's hash. Leht does
    // not need the keying; the data is the same kind.
    pdf_obj* vri = nullptr;
    int n = 0;
    guarded(c, [&](fz_context* g) {
        vri = pdf_dict_gets(g, dss, "VRI");
        n = pdf_dict_len(g, vri);
    });
    for (int i = 0; i < n && r.items < kMaxItems; ++i) {
        pdf_obj* entry = nullptr;
        guarded(c, [&](fz_context* g) { entry = pdf_dict_get_val(g, vri, i); });
        r.list(entry, "Cert", out.certs);
        r.list(entry, "OCSP", out.ocsps);
        r.list(entry, "CRL", out.crls);
    }
    return out;
}

void add_validation_data(const Context& ctx, Document& doc, const ValidationData& data, int fd) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    if (!doc.can_save_incrementally()) {
        throw Error(0, "this document was repaired when opened or redacted; validation data "
                       "can only be appended to a file saved as it is");
    }
    // What is there already, so nothing is stored twice.
    const ValidationData have = read_dss(ctx, doc);
    std::set<Blob> known;
    for (const auto* list : {&have.certs, &have.ocsps, &have.crls}) {
        known.insert(list->begin(), list->end());
    }

    pdf_obj* dss = nullptr;
    guarded(c, [&](fz_context* g) {
        pdf_obj* root = pdf_dict_get(g, pdf_trailer(g, pdf), PDF_NAME(Root));
        dss = pdf_dict_gets(g, root, "DSS");
        if (!pdf_is_dict(g, dss)) {
            pdf_dict_puts_drop(g, root, "DSS", pdf_add_new_dict(g, pdf, 4));
            dss = pdf_dict_gets(g, root, "DSS");
        }
        pdf_dict_put_name(g, dss, PDF_NAME(Type), "DSS");
    });
    for (const auto& [key, items] : {std::pair{"Certs", &data.certs},
                                     std::pair{"OCSPs", &data.ocsps},
                                     std::pair{"CRLs", &data.crls}}) {
        if (items->empty()) {
            continue;
        }
        pdf_obj* array = nullptr;
        guarded(c, [&](fz_context* g) { array = array_for(g, pdf, dss, key); });
        for (const Blob& item : *items) {
            if (!known.insert(item).second) {
                continue;
            }
            detail::OwnedBuffer buf{c};
            guarded(c, [&](fz_context* g) {
                *buf.slot() = fz_new_buffer_from_copied_data(g, item.data(), item.size());
                pdf_array_push_drop(g, array, pdf_add_stream(g, pdf, buf.get(), nullptr, 0));
            });
        }
    }

    SaveOptions opts;
    opts.mode = SaveOptions::Mode::Incremental;
    doc.save_fd(fd, opts);
}

}  // namespace leht::ops
