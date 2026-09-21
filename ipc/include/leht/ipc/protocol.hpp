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

#include "leht/ipc/wire.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace leht::ipc {

/// Bumped on any change to framing or to a message layout. Peers exchange it
/// in Hello/HelloAck, and a mismatch ends the connection.
inline constexpr std::uint32_t kProtocolVersion = 1;

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
};

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

/// The document itself travels as the frame's attached fd.
struct Open {
    static constexpr MsgType kType = MsgType::Open;
    void encode(Writer&) const {}
    static Open decode(Reader&) { return {}; }
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

struct Search {
    static constexpr MsgType kType = MsgType::Search;
    std::string needle;
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

}  // namespace leht::ipc
