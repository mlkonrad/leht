// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The messages exchanged between the viewer and leht-worker.
//
// Every payload is a serialization of a plain type core/ already exposes --
// Bitmap, PageSize, TextQuad, outline entries, strings. No MuPDF type, pointer
// or parser state crosses the process boundary; that is the whole point of the
// split, and it is why core/ has never let a MuPDF type into a public header.
//
// Decoding validates semantics as well as bounds: a page index is non-negative,
// a rotation is a multiple of 90, a bitmap's stride and height account for
// exactly the bytes it carries. The receiving side can then use a decoded
// message without re-checking it.

#include "leht/edit.hpp"
#include "leht/ipc/wire.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/crop.hpp"
#include "leht/ops/forms.hpp"
#include "leht/ops/sign.hpp"
#include "leht/ops/watermark.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace leht::ipc {

/// Bumped on any change to framing or to a message layout. Peers exchange it
/// in Hello/HelloAck, and a mismatch ends the connection.
inline constexpr std::uint32_t kProtocolVersion = 5;  // 2: CancelSearch; 3: editing; 4: signatures; 5: move, retext, crop box

/// Largest payload either side will accept. Comfortably above the biggest
/// legitimate message (a rendered page) and far below anything that would let
/// a hostile peer exhaust memory with one length field.
inline constexpr std::size_t kMaxPayload = std::size_t{256} << 20;

/// Longest string field: titles, passwords, search needles, selection text.
inline constexpr std::size_t kMaxString = std::size_t{16} << 20;

enum class MsgType : std::uint16_t {
    // viewer -> worker
    Hello = 1,
    Open = 2,          ///< carries the document's fd via SCM_RIGHTS
    Authenticate = 3,
    Render = 4,
    Cancel = 5,
    Search = 6,
    Select = 7,
    Shutdown = 8,
    CancelSearch = 9,
    Edit = 10,
    Save = 11,         ///< carries the output file's fd via SCM_RIGHTS
    ListAnnots = 12,
    ListFields = 13,
    PrepareSignature = 14,  ///< carries the output file's fd via SCM_RIGHTS
    ListSignatures = 15,

    // worker -> viewer
    HelloAck = 100,
    NeedsPassword = 101,
    Opened = 102,
    Outline = 103,
    Rendered = 104,
    RenderSkipped = 105,
    PageMatches = 106,
    SearchDone = 107,
    SelectionResult = 108,
    Failed = 109,
    Edited = 110,
    Saved = 111,
    AnnotList = 112,
    FieldList = 113,
    SignaturePrepared = 114,
    SignatureList = 115,
};

/// Whether a frame of `type` may carry a file descriptor: only those that hand
/// the worker a file to read (Open) or to write (Save, PrepareSignature).
[[nodiscard]] bool takes_fd(MsgType type) noexcept;

/// True for every value in MsgType. Frames with any other type are rejected
/// before their payload is read.
[[nodiscard]] bool is_known(std::uint16_t type) noexcept;

// --- viewer -> worker --------------------------------------------------------

struct Hello {
    static constexpr MsgType kType = MsgType::Hello;
    std::uint32_t version = kProtocolVersion;
    void encode(Writer& w) const;
    static Hello decode(Reader& r);
};

/// The document itself travels as the frame's attached fd. `name` is only a
/// format hint (the file's base name, so MuPDF can pick a handler by
/// extension); the worker never opens anything by name.
struct Open {
    static constexpr MsgType kType = MsgType::Open;
    std::string name;
    void encode(Writer& w) const;
    static Open decode(Reader& r);
};

struct Authenticate {
    static constexpr MsgType kType = MsgType::Authenticate;
    std::string password;
    void encode(Writer& w) const;
    static Authenticate decode(Reader& r);
};

struct Render {
    static constexpr MsgType kType = MsgType::Render;
    int page = 0;
    float zoom = 1.0F;
    int rotation = 0;
    std::uint64_t generation = 0;
    void encode(Writer& w) const;
    static Render decode(Reader& r);
};

/// Everything older than `generation` is stale: queued renders are dropped
/// and an in-flight one is aborted.
struct Cancel {
    static constexpr MsgType kType = MsgType::Cancel;
    std::uint64_t generation = 0;
    void encode(Writer& w) const;
    static Cancel decode(Reader& r);
};

/// `epoch` identifies the search for CancelSearch.
struct Search {
    static constexpr MsgType kType = MsgType::Search;
    std::string needle;
    std::uint64_t epoch = 0;
    void encode(Writer& w) const;
    static Search decode(Reader& r);
};

/// Points in base coordinates (page pixels at zoom 1.0).
struct Select {
    static constexpr MsgType kType = MsgType::Select;
    int page = 0;
    float ax = 0, ay = 0, bx = 0, by = 0;
    SelectMode mode = SelectMode::Chars;
    void encode(Writer& w) const;
    static Select decode(Reader& r);
};

/// Cancels every search with an epoch older than `epoch`: a running one stops
/// at the next page, a queued one ends at once. Either still ends with its
/// SearchDone (matches so far), so the stream stays in step. Numbered rather
/// than a flag so that a cancel can never be lost to a search that had not
/// yet started, nor stop one that starts after it.
struct CancelSearch {
    static constexpr MsgType kType = MsgType::CancelSearch;
    std::uint64_t epoch = 0;
    void encode(Writer& w) const;
    static CancelSearch decode(Reader& r);
};

/// One editing operation, applied to the open document. `kind` selects which
/// of the fields below are meaningful; the rest are left at their defaults
/// and not sent. Coordinates are base coordinates (see leht::Rect).
///
/// The viewer keeps every Edit it has sent since the last save: undo reopens
/// the file and replays all but the last, and a respawned worker gets the
/// whole list. So an Edit must mean the same thing every time it is applied.
struct Edit {
    static constexpr MsgType kType = MsgType::Edit;
    enum class Kind : std::uint8_t {
        Redact = 1,       ///< page, rects
        RedactText = 2,   ///< text: the needle, over the whole document
        AddAnnot = 3,     ///< page, annot
        DeleteAnnot = 4,  ///< annot_id
        SetField = 5,     ///< name, text: the value
        Watermark = 6,    ///< pages, watermark
        CropMargins = 7,  ///< pages, margins
        MoveAnnot = 8,    ///< annot_id, rects[0]: its new bounds
        SetAnnotContents = 9,  ///< annot_id, text
        CropBox = 10,     ///< pages, rects[0]: the box to keep
    };
    Kind kind = Kind::Redact;
    int page = 0;
    std::vector<Rect> rects;
    std::string text;
    std::string name;
    std::string pages;  ///< a page-range spec; empty means all
    ops::AnnotSpec annot;
    int annot_id = 0;
    ops::WatermarkOptions watermark;
    ops::Margins margins;
    void encode(Writer& w) const;
    static Edit decode(Reader& r);
};

/// Writes the document, with every edit applied, into the frame's attached
/// fd (a fresh temporary file the viewer created). The viewer makes it
/// durable and renames it into place.
struct Save {
    static constexpr MsgType kType = MsgType::Save;
    void encode(Writer&) const {}
    static Save decode(Reader&) { return {}; }
};

struct ListAnnots {
    static constexpr MsgType kType = MsgType::ListAnnots;
    void encode(Writer&) const {}
    static ListAnnots decode(Reader&) { return {}; }
};

struct ListFields {
    static constexpr MsgType kType = MsgType::ListFields;
    void encode(Writer&) const {}
    static ListFields decode(Reader&) { return {}; }
};

/// Write the document plus a signature with an empty hole into the attached
/// descriptor. The worker never sees the key: the viewer fills the hole in
/// afterwards, having checked it against the bytes.
struct PrepareSignature {
    static constexpr MsgType kType = MsgType::PrepareSignature;
    ops::SignatureRequest request;
    void encode(Writer& w) const;
    static PrepareSignature decode(Reader& r);
};

/// List the signatures and verify them -- in the worker, because the CMS blobs
/// come out of the document and are therefore hostile input.
///
/// `trust_pem` is the certificates to trust, as PEM: the viewer reads the
/// system store (the worker cannot open files) and adds the user's own.
struct ListSignatures {
    static constexpr MsgType kType = MsgType::ListSignatures;
    std::string trust_pem;
    void encode(Writer& w) const;
    static ListSignatures decode(Reader& r);
};

struct Shutdown {
    static constexpr MsgType kType = MsgType::Shutdown;
    void encode(Writer&) const {}
    static Shutdown decode(Reader&) { return {}; }
};

// --- worker -> viewer --------------------------------------------------------

struct HelloAck {
    static constexpr MsgType kType = MsgType::HelloAck;
    std::uint32_t version = kProtocolVersion;
    void encode(Writer& w) const;
    static HelloAck decode(Reader& r);
};

/// The document is encrypted and held open awaiting Authenticate. `retry` is
/// true when a password was just tried and was wrong.
struct NeedsPassword {
    static constexpr MsgType kType = MsgType::NeedsPassword;
    bool retry = false;
    void encode(Writer& w) const;
    static NeedsPassword decode(Reader& r);
};

/// Page sizes are at zoom 1.0, rotation 0: one per page.
struct Opened {
    static constexpr MsgType kType = MsgType::Opened;
    std::vector<PageSize> base_sizes;
    void encode(Writer& w) const;
    static Opened decode(Reader& r);
};

/// One outline entry, flattened: `depth` replaces the tree's nesting, the
/// same shape the viewer's outline sidebar consumes.
struct OutlineRow {
    int depth = 0;
    std::string title;
    int page = -1;
    float y = 0.0F;
};

struct Outline {
    static constexpr MsgType kType = MsgType::Outline;
    std::vector<OutlineRow> rows;
    void encode(Writer& w) const;
    static Outline decode(Reader& r);
};

struct Rendered {
    static constexpr MsgType kType = MsgType::Rendered;
    int page = 0;
    float zoom = 1.0F;
    int rotation = 0;
    std::uint64_t generation = 0;
    Bitmap bitmap;  ///< always RGB (3 channels)
    void encode(Writer& w) const;
    static Rendered decode(Reader& r);
};

/// The render produced no image: stale, cancelled, or the page failed. Every
/// Render gets exactly one Rendered or RenderSkipped, so a caller waiting on
/// one never waits forever.
struct RenderSkipped {
    static constexpr MsgType kType = MsgType::RenderSkipped;
    int page = 0;
    std::uint64_t generation = 0;
    void encode(Writer& w) const;
    static RenderSkipped decode(Reader& r);
};

/// All matches on one page, in base coordinates. One quad per matched line.
struct PageMatches {
    static constexpr MsgType kType = MsgType::PageMatches;
    int page = 0;
    std::vector<TextQuad> quads;
    void encode(Writer& w) const;
    static PageMatches decode(Reader& r);
};

struct SearchDone {
    static constexpr MsgType kType = MsgType::SearchDone;
    std::uint32_t total = 0;
    void encode(Writer& w) const;
    static SearchDone decode(Reader& r);
};

struct SelectionResult {
    static constexpr MsgType kType = MsgType::SelectionResult;
    int page = 0;
    std::vector<TextQuad> quads;
    std::string text;
    void encode(Writer& w) const;
    static SelectionResult decode(Reader& r);
};

/// A request failed with an ordinary error (not a crash): unreadable file,
/// bad page. `message` is for display.
struct Failed {
    static constexpr MsgType kType = MsgType::Failed;
    std::string message;
    void encode(Writer& w) const;
    static Failed decode(Reader& r);
};

/// An edit was applied. The viewer drops its rendered images of the pages
/// named (every page, if `all_pages`), and takes `base_sizes` as the new page
/// sizes: a crop changes them.
struct Edited {
    static constexpr MsgType kType = MsgType::Edited;
    bool all_pages = false;
    std::vector<int> pages;
    std::vector<PageSize> base_sizes;
    int annot_id = 0;  ///< AddAnnot: the new annotation's id
    /// RedactText: where the text still appears (see ops::RedactResult).
    std::vector<std::string> remaining;
    void encode(Writer& w) const;
    static Edited decode(Reader& r);
};

struct Saved {
    static constexpr MsgType kType = MsgType::Saved;
    std::uint64_t bytes = 0;
    void encode(Writer& w) const;
    static Saved decode(Reader& r);
};

struct AnnotList {
    static constexpr MsgType kType = MsgType::AnnotList;
    std::vector<ops::AnnotInfo> items;
    void encode(Writer& w) const;
    static AnnotList decode(Reader& r);
};

struct FieldList {
    static constexpr MsgType kType = MsgType::FieldList;
    std::vector<ops::FieldInfo> items;
    void encode(Writer& w) const;
    static FieldList decode(Reader& r);
};

struct SignaturePrepared {
    static constexpr MsgType kType = MsgType::SignaturePrepared;
    ops::ByteRange range;
    std::string field;
    void encode(Writer& w) const;
    static SignaturePrepared decode(Reader& r);
};

/// A certificate, as much of it as the viewer displays.
struct CertRow {
    std::string subject, common_name, issuer, serial, sha256;
    std::int64_t not_before = 0, not_after = 0;
    bool is_ca = false;
    bool can_sign = true;
};

/// One signature: what the document claims, and what verification made of it.
///
/// The CMS blob itself stays in the worker. Nothing here is parsed further by
/// the viewer, which is the point: the viewer shows strings and flags.
struct SignatureRow {
    // From the document.
    std::string field;
    int page = -1;
    Rect rect;
    std::string subfilter, name, reason, location, claimed_time;
    bool range_ok = false;
    std::string range_problem;
    bool covers_whole_revision = false;
    bool changed_after_signing = false;
    bool later_signature_covers_changes = false;

    // From verification. `checked` is false when the byte range made
    // verification pointless.
    bool checked = false;
    bool intact = false;
    std::string problem;
    std::string digest;
    CertRow signer;
    std::vector<CertRow> chain;
    /// crypto::Trust, as its underlying value.
    std::uint8_t trust = 0;
    std::string trust_detail;
    bool has_signing_certificate_v2 = false;
    bool has_signing_time_attribute = false;

    bool has_timestamp = false;
    bool timestamp_valid = false;
    std::int64_t timestamp_time = 0;
    CertRow authority;
    std::uint8_t timestamp_trust = 0;
    std::string timestamp_problem;
};

struct SignatureList {
    static constexpr MsgType kType = MsgType::SignatureList;
    std::vector<SignatureRow> rows;
    void encode(Writer& w) const;
    static SignatureList decode(Reader& r);
};

}  // namespace leht::ipc
