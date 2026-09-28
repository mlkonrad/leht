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
#include "leht/ops/ocr_layer.hpp"
#include "leht/ops/sign.hpp"
#include "leht/ops/watermark.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace leht::ipc {

/// Bumped on any change to framing or to a message layout. Peers exchange it
/// in Hello/HelloAck, and a mismatch ends the connection.
inline constexpr std::uint32_t kProtocolVersion = 11;  // 2: CancelSearch; 3: editing; 4: signatures; 5: move, retext, crop box; 6: OCR; 7: certification; 8: long-term validation; 9: merge, compress, split; 10: trusted lists; 11: organising pages

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
    Recognize = 16,    ///< to the OCR worker: pixels to read
    ListTextPages = 17,
    ListRevocationQueries = 18,
    AddValidationData = 19,    ///< carries the output file's fd via SCM_RIGHTS
    PrepareDocTimestamp = 20,  ///< carries the output file's fd via SCM_RIGHTS

    // File tools (merge, compress, split): requests 21-29.
    Compress = 21,     ///< carries the output file's fd via SCM_RIGHTS
    ExtractPages = 22, ///< carries the output file's fd via SCM_RIGHTS
    MergeBegin = 23,
    MergeAdd = 24,     ///< carries an INPUT file's fd via SCM_RIGHTS
    MergeFinish = 25,  ///< carries the output file's fd via SCM_RIGHTS
    Protect = 26,      ///< carries the output file's fd via SCM_RIGHTS
    GetInfo = 27,
    ExtractText = 28,

    // The EU trusted lists: requests 30-39, to leht-worker --trusted-list.
    TrustedListStep = 30,

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
    Words = 116,       ///< from the OCR worker: what it read
    TextPageList = 117,
    RevocationQueryList = 118,
    ValidationDataAdded = 119,

    // File tools: replies 120-129.
    Compressed = 120,
    PagesWritten = 121,
    MergeAdded = 122,
    Merged = 123,
    Protected = 124,
    DocInfo = 125,
    DocText = 126,

    // The EU trusted lists: replies 130-139.
    TrustedListProgress = 130,
};

/// Whether a frame of `type` may carry a file descriptor: only those that hand
/// the worker a file to read (Open, MergeAdd) or to write (Save,
/// PrepareSignature, AddValidationData, PrepareDocTimestamp, Compress,
/// ExtractPages, MergeFinish, Protect).
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
        AddTextLayer = 11,  ///< page, words: an OCR'd page's invisible text
        // Organising pages (ops/organize.hpp). The pages change, so Edited
        // says all_pages and carries the new sizes.
        RotatePages = 12,   ///< pages, degrees
        DeletePages = 13,   ///< pages
        MovePages = 14,     ///< pages, page: the one they go before (page count: the end)
        InsertPages = 15,   ///< page: where; data: the source file; pages: its pages ("": all)
        InsertBlank = 16,   ///< page: where; rects[0]: (0, 0, width, height)
        SetInfo = 17,       ///< name: Title, Author, Subject or Keywords; text: the value ("" removes)
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
    std::vector<ops::OcrWord> words;
    int degrees = 0;
    /// InsertPages: the whole source file, read by the viewer and parsed only
    /// here. Carried in the edit, not named by path, so replaying the log
    /// inserts the same pages even if the file has changed since.
    std::vector<std::uint8_t> data;
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

/// To the OCR worker: read the text in `bitmap`, a page rendered at `zoom`
/// (pixels per point), and give the words back in base coordinates.
struct Recognize {
    static constexpr MsgType kType = MsgType::Recognize;
    float zoom = 1.0F;
    Bitmap bitmap;  ///< RGB (3 channels)
    void encode(Writer& w) const;
    static Recognize decode(Reader& r);
};

/// Which pages already carry text (see ops::page_has_text): OCR leaves them.
struct ListTextPages {
    static constexpr MsgType kType = MsgType::ListTextPages;
    void encode(Writer&) const {}
    static ListTextPages decode(Reader&) { return {}; }
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
/// A revocation response the viewer fetched, passed on unread: the worker
/// parses it. `kind` is crypto::RevocationQuery::Kind's value.
struct FetchedRow {
    std::uint8_t kind = 0;
    std::string url;
    std::vector<std::uint8_t> body;  ///< empty when the fetch failed
    std::string error;
};

/// Long-term validation, in three steps so that the worker parses and the
/// viewer only moves bytes: ListRevocationQueries (what to fetch), the viewer
/// fetches, AddValidationData (the worker picks what to keep and appends the
/// /DSS revision). Then, optionally, PrepareDocTimestamp: a hole the viewer
/// fills with a token from the TSA, as it fills a signature's.
struct ListRevocationQueries {
    static constexpr MsgType kType = MsgType::ListRevocationQueries;
    std::string trust_pem;
    void encode(Writer& w) const;
    static ListRevocationQueries decode(Reader& r);
};

struct AddValidationData {
    static constexpr MsgType kType = MsgType::AddValidationData;
    std::string trust_pem;
    std::vector<FetchedRow> fetched;
    void encode(Writer& w) const;
    static AddValidationData decode(Reader& r);
};

struct PrepareDocTimestamp {
    static constexpr MsgType kType = MsgType::PrepareDocTimestamp;
    std::uint32_t reserve = 16384;
    void encode(Writer& w) const;
    static PrepareDocTimestamp decode(Reader& r);
};

/// One step of updating the EU trusted lists, in leht-worker --trusted-list:
/// the LOTL and everything fetched so far, as bytes the viewer did not read.
/// The worker runs trustlist::advance() and says what to fetch next, or
/// returns the verified result in Leht's compact form.
struct TrustedListStep {
    static constexpr MsgType kType = MsgType::TrustedListStep;
    std::vector<std::uint8_t> lotl;
    std::vector<FetchedRow> fetched;  ///< `kind` unused
    void encode(Writer& w) const;
    static TrustedListStep decode(Reader& r);
};

struct TrustedListProgress {
    static constexpr MsgType kType = MsgType::TrustedListProgress;
    std::vector<std::string> need;  ///< http(s) URLs to fetch next; empty when done
    bool done = false;
    /// When done and the LOTL verified: trustlist::encode() of the result.
    std::vector<std::uint8_t> blob;
    std::string problem;  ///< when done without a verified LOTL: why
    void encode(Writer& w) const;
    static TrustedListProgress decode(Reader& r);
};

struct ListSignatures {
    static constexpr MsgType kType = MsgType::ListSignatures;
    std::string trust_pem;
    /// Responses fetched just now (Check Revocation Online), to check against
    /// along with the document's own; not embedded.
    std::vector<FetchedRow> online{};
    /// The cached EU trusted lists (trustlist::encode), or empty: they then
    /// anchor trust and decide the "qualified" verdicts.
    std::vector<std::uint8_t> trusted_list{};
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

// --- file tools: merge, compress, split ---------------------------------------
//
// These run in a worker of their own, never in the one showing the document:
// compress rewrites its document in place, and a hostile input to a merge
// should cost that job, not the open document. Compress and ExtractPages act
// on the document that worker opened (Open, and Authenticate if need be), as
// it is on disk; a merge needs no Open at all.

/// Recompresses the open document into the attached fd (ops::compress).
struct Compress {
    static constexpr MsgType kType = MsgType::Compress;
    std::uint8_t preset = 2;          ///< ops::CompressPreset: 0 lossless .. 3 screen
    std::uint32_t jpeg_quality = 0;   ///< 0 = the preset's; else 1-100
    std::uint32_t max_image_edge = 0; ///< 0 = the preset's; else pixels
    bool linearize = false;
    void encode(Writer& w) const;
    static Compress decode(Reader& r);
};

/// Writes the pages `ranges` names (ops::parse_page_ranges syntax) of the open
/// document into the attached fd. The viewer splits by sending one per part.
struct ExtractPages {
    static constexpr MsgType kType = MsgType::ExtractPages;
    std::string ranges;
    void encode(Writer& w) const;
    static ExtractPages decode(Reader& r);
};

/// Starts a merge (ops::Merger), dropping any unfinished one. Answered with
/// MergeAdded{0}.
struct MergeBegin {
    static constexpr MsgType kType = MsgType::MergeBegin;
    bool linearize = false;
    void encode(Writer& w) const;
    static MergeBegin decode(Reader& r);
};

/// Appends the attached file, a PDF or an image, to the merge. `name` is for
/// error messages only; the worker goes by the content.
struct MergeAdd {
    static constexpr MsgType kType = MsgType::MergeAdd;
    std::string name;
    void encode(Writer& w) const;
    static MergeAdd decode(Reader& r);
};

/// Writes the merge into the attached fd and ends it.
struct MergeFinish {
    static constexpr MsgType kType = MsgType::MergeFinish;
    void encode(Writer&) const {}
    static MergeFinish decode(Reader&) { return {}; }
};

/// Writes the open (unlocked) document into the attached fd with a password
/// (ops::encrypt) or without any (ops::decrypt). The passwords are the new
/// file's, not the document's: the worker needs them to write it.
struct Protect {
    static constexpr MsgType kType = MsgType::Protect;
    bool encrypt = true;           ///< false: remove the password
    std::string user_password;     ///< to open; may be empty (permissions only)
    std::string owner_password;    ///< full rights; empty reuses the user's
    std::uint8_t method = 2;       ///< ops::Encryption: 0 RC4-128, 1 AES-128, 2 AES-256
    /// ops::Permissions, one bit each in declaration order: print, modify,
    /// copy, annotate, fill_forms, assemble, print_high_quality.
    std::uint8_t permissions = 0x71;
    void encode(Writer& w) const;
    static Protect decode(Reader& r);
};

/// The document's descriptive facts, for File > Document Properties.
struct GetInfo {
    static constexpr MsgType kType = MsgType::GetInfo;
    void encode(Writer&) const {}
    static GetInfo decode(Reader&) { return {}; }
};

/// Answers GetInfo: Document::metadata() keys ("format", "encryption",
/// "info:Title", ...) with their values, only those the document has.
struct DocInfo {
    static constexpr MsgType kType = MsgType::DocInfo;
    std::vector<std::pair<std::string, std::string>> fields;
    void encode(Writer& w) const;
    static DocInfo decode(Reader& r);
};

/// File > Export > Text: the text of `pages` (a range spec; "" all), in
/// reading order, one string per page.
struct ExtractText {
    static constexpr MsgType kType = MsgType::ExtractText;
    std::string pages;
    void encode(Writer& w) const;
    static ExtractText decode(Reader& r);
};

struct DocText {
    static constexpr MsgType kType = MsgType::DocText;
    std::vector<int> page_numbers;    ///< 0-based, as the pages were asked for
    std::vector<std::string> texts;   ///< one per page number
    void encode(Writer& w) const;
    static DocText decode(Reader& r);
};

struct Protected {
    static constexpr MsgType kType = MsgType::Protected;
    std::uint64_t bytes = 0;
    void encode(Writer& w) const;
    static Protected decode(Reader& r);
};

struct Compressed {
    static constexpr MsgType kType = MsgType::Compressed;
    std::uint64_t bytes = 0;
    std::uint32_t images_examined = 0;
    std::uint32_t images_recompressed = 0;
    void encode(Writer& w) const;
    static Compressed decode(Reader& r);
};

struct PagesWritten {
    static constexpr MsgType kType = MsgType::PagesWritten;
    std::uint32_t pages = 0;
    std::uint64_t bytes = 0;
    void encode(Writer& w) const;
    static PagesWritten decode(Reader& r);
};

/// One MergeAdd succeeded, adding `pages`.
struct MergeAdded {
    static constexpr MsgType kType = MsgType::MergeAdded;
    std::uint32_t pages = 0;
    void encode(Writer& w) const;
    static MergeAdded decode(Reader& r);
};

struct Merged {
    static constexpr MsgType kType = MsgType::Merged;
    std::uint32_t inputs = 0;
    std::uint32_t pages = 0;
    std::uint64_t bytes = 0;
    void encode(Writer& w) const;
    static Merged decode(Reader& r);
};

/// From the OCR worker: the words it read, in base coordinates.
struct Words {
    static constexpr MsgType kType = MsgType::Words;
    std::vector<ops::OcrWord> words;
    void encode(Writer& w) const;
    static Words decode(Reader& r);
};

struct TextPageList {
    static constexpr MsgType kType = MsgType::TextPageList;
    std::vector<int> pages;  ///< 0-based, ascending
    void encode(Writer& w) const;
    static TextPageList decode(Reader& r);
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

/// One certificate's revocation status (crypto::RevocationCheck).
struct RevocationRow {
    std::string subject;  ///< its common name, or the whole subject
    std::uint8_t status = 2;  ///< crypto::RevocationStatus: 0 good, 1 revoked, 2 unknown
    std::string source;
    std::int64_t data_time = 0;
    std::int64_t revoked_at = 0;
    std::string problem;
};

/// What to fetch: crypto::RevocationQuery.
struct QueryRow {
    std::uint8_t kind = 0;  ///< 0 OCSP (POST `request`), 1 CRL (GET)
    std::string url;
    std::vector<std::uint8_t> request;
    std::string subject;
};

struct RevocationQueryList {
    static constexpr MsgType kType = MsgType::RevocationQueryList;
    std::vector<QueryRow> queries;
    void encode(Writer& w) const;
    static RevocationQueryList decode(Reader& r);
};

/// How much a /DSS revision added; all zero when there was nothing new, and
/// then nothing was written.
struct ValidationDataAdded {
    static constexpr MsgType kType = MsgType::ValidationDataAdded;
    std::uint32_t certs = 0, ocsps = 0, crls = 0;
    void encode(Writer& w) const;
    static ValidationDataAdded decode(Reader& r);
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
    // Certification and locks (see ops::SignatureInfo).
    std::uint8_t certification = 0;
    std::string locks;
    bool changes_judged = false;
    bool changes_permitted = true;
    std::vector<std::string> change_problems;

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

    // Long-term validation. A document timestamp fills the timestamp fields
    // and `intact`; it has no signer of its own.
    bool document_timestamp = false;
    bool only_validation_data_after = false;
    /// Empty when no revocation data was there to check against.
    std::vector<RevocationRow> revocation;
    std::vector<RevocationRow> timestamp_revocation;

    // The trusted lists' verdicts (crypto::QualifiedReport; Level as u8):
    // the signer's, and its timestamp's. 0 is "not checked".
    std::uint8_t qualified = 0;
    std::string qualified_service, qualified_territory, qualified_detail;
    std::uint8_t timestamp_qualified = 0;
    std::string timestamp_qualified_detail;
};

struct SignatureList {
    static constexpr MsgType kType = MsgType::SignatureList;
    std::vector<SignatureRow> rows;
    void encode(Writer& w) const;
    static SignatureList decode(Reader& r);
};

}  // namespace leht::ipc
