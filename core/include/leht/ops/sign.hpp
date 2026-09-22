// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The PDF side of signing. MuPDF only: the cryptography lives in leht::crypto,
// and the two meet at ByteRange.
//
// Signing is split in two, so that the process holding the private key never
// parses the PDF and the process parsing the PDF never holds the key:
//
//   1. prepare_signature() (here, in the worker for the viewer) writes the
//      document plus a new revision holding the signature dictionary, whose
//      /Contents is a hole of zeros, and reports the /ByteRange around it.
//   2. crypto::sign_prepared() (in the trusted process) checks the hole on
//      the file's own bytes, signs everything around it, and fills it in.

#include "leht/edit.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

/// A signature's /ByteRange: the two spans of the file it signs, which are
/// everything except the hole holding the signature itself (/Contents, as a
/// hex string with its angle brackets).
///
///     [ offset0 length0 offset1 length1 ]   offset0 == 0
///       hole = [offset0 + length0, offset1)  -- starts '<', ends '>'
struct ByteRange {
    std::array<std::int64_t, 4> v{};

    [[nodiscard]] std::int64_t hole_begin() const { return v[0] + v[1]; }
    [[nodiscard]] std::int64_t hole_end() const { return v[2]; }
    /// Where the signed bytes end: the end of the revision the signature covers.
    [[nodiscard]] std::int64_t end() const { return v[2] + v[3]; }
};

/// What a visible signature (or a signature stamp) shows. The graphic -- an
/// image, or strokes drawn by hand -- takes the left half when there is text,
/// the whole box otherwise; the text lines take the right half.
struct Appearance {
    /// PNG or JPEG bytes. Decoded by MuPDF, so in the viewer this happens in
    /// the sandboxed worker, like any other untrusted file.
    std::vector<std::uint8_t> image;
    /// A drawn signature: strokes in a canvas `strokes_width` x
    /// `strokes_height` (any unit, origin top-left), scaled to fit.
    std::vector<std::vector<Point>> strokes;
    float strokes_width = 0;
    float strokes_height = 0;
    float stroke_width = 2.0F;  ///< in canvas units
    /// Text, one entry per line: typically the signer's name and the date.
    std::vector<std::string> lines;

    [[nodiscard]] bool empty() const {
        return image.empty() && strokes.empty() && lines.empty();
    }
};

struct SignatureRequest {
    /// Sign this existing, unsigned signature field (its full name). Empty:
    /// create a new field, named "Signature<n>".
    std::string field;
    /// For a new field: the page (0-based) and box, in base coordinates. An
    /// empty box makes an invisible signature.
    int page = 0;
    Rect rect;
    Appearance appearance;
    std::string name;      ///< /Name: the signer, as the dictionary claims it
    std::string reason;    ///< /Reason
    std::string location;  ///< /Location
    /// /M, Unix seconds; 0 means now. The claimed signing time: only a
    /// timestamp (PAdES B-T) proves when a signature existed.
    std::int64_t time = 0;
    /// Bytes of DER the hole must hold: crypto::estimate_signature_size().
    std::size_t reserve = 16384;
};

struct PreparedSignature {
    ByteRange range;
    std::string field;  ///< the field signed
};

/// Adds the signature dictionary and writes the whole document, as an
/// incremental update, into `fd` (which must be empty and open read-write).
/// Any edits made to `doc` before are saved in the same revision, so the
/// signature covers them.
///
/// The dictionary is PAdES-shaped: /SubFilter /ETSI.CAdES.detached, and no
/// FieldMDP transform -- M5 signs a document, it does not lock its fields, and
/// a transform that locks nothing is a claim a validator has to read. A field's
/// existing /Lock is likewise not enacted. The hole is filled later by
/// crypto::sign_prepared(); until then the file carries an invalid signature.
///
/// Like any incremental save, this leaves `doc` unable to save again: reopen
/// the file. Throws leht::Error on a non-PDF, a document that cannot be saved
/// incrementally (repaired when opened, or redacted -- save it in full first),
/// an unknown or already signed field, or a bad page or box.
PreparedSignature prepare_signature(const Context& ctx, Document& doc,
                                    const SignatureRequest& request, int fd);

/// A signature as the document states it. None of this is verified: the
/// cryptography is crypto::verify_cms() over `contents` and the bytes
/// `range` names.
struct SignatureInfo {
    std::string field;
    int page = -1;  ///< -1 for an invisible signature not on any page
    Rect rect;
    std::string filter;     ///< /Filter, e.g. "Adobe.PPKLite"
    std::string subfilter;  ///< "ETSI.CAdES.detached", "adbe.pkcs7.detached", ...
    std::string name, reason, location;
    std::string claimed_time;  ///< /M as written, e.g. "D:20260922121500+03'00'"
    std::vector<std::uint8_t> contents;  ///< the CMS blob, zero padding included

    /// The /ByteRange is two spans, starts at 0, lies within the file and
    /// leaves exactly the /Contents string out. False means the signature
    /// cannot be meaningfully checked, whatever the cryptography says.
    bool range_ok = false;
    ByteRange range;
    std::string range_problem;
    /// The signed bytes end at the end of a revision (a %%EOF).
    bool covers_whole_revision = false;
    /// Bytes were added after the signed revision: later updates exist. The
    /// signature can still be intact -- it covers what it covered -- but what
    /// the document shows now is not only what was signed.
    ///
    /// There is deliberately no "were those changes permitted?" flag.
    /// MuPDF's pdf_validate_signature() answers that only in terms of field
    /// locks, and with none set it reports a plain second signature exactly as
    /// it reports a watermark stamped over the text. A flag that cries wolf on
    /// the commonest case is worse than none.
    bool changed_after_signing = false;
    /// Another signature in this document signs those later bytes too (its
    /// range reaches further than this one's). Whether that signature is
    /// itself intact is for the caller to check -- and only then does this
    /// mean "the changes are signed for as well".
    bool later_signature_covers_changes = false;
};

/// Every signed signature field, in field order. Unsigned signature fields are
/// not listed (list_fields() has them).
std::vector<SignatureInfo> list_signatures(const Context& ctx, Document& doc);

/// Streams the bytes a ByteRange covers, from the file the document was opened
/// from: what a verifier digests. Each call fills the buffer and returns the
/// count, 0 at the end. The ByteRange must be one SignatureInfo reported as
/// `range_ok`.
using ByteReader = std::function<std::size_t(std::uint8_t* buffer, std::size_t size)>;
ByteReader signed_bytes(const Context& ctx, const Document& doc, const ByteRange& range);

/// A visible signature mark that is NOT a cryptographic signature: a Stamp
/// annotation showing `appearance` in `rect` on `page`. It proves nothing about
/// who made it, and its /Contents says so. Returns its annotation id.
int add_signature_stamp(const Context& ctx, Document& doc, int page, const Rect& rect,
                        const Appearance& appearance);

}  // namespace leht::ops
