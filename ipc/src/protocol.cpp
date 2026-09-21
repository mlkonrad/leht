// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ipc/protocol.hpp"

#include <cstddef>
#include <limits>

namespace leht::ipc {

namespace {

// Semantic limits. They are generous -- the point is to reject values no
// legitimate peer produces, not to second-guess real documents.
constexpr int kMaxPage = 10'000'000;
constexpr int kMaxEdge = 1 << 16;      ///< widest bitmap or base page, in px
constexpr float kMaxZoom = 64.0F;
constexpr float kMaxCoord = 1.0e7F;    ///< any coordinate on any page
constexpr int kMaxDepth = 4096;
constexpr std::size_t kQuadBytes = 8 * 4;

int page_index(Reader& r) {
    const int p = r.i32();
    if (p < 0 || p > kMaxPage) {
        throw ProtocolError("page index out of range");
    }
    return p;
}

float zoom(Reader& r) {
    const float z = r.f32();
    if (!(z > 0.0F) || z > kMaxZoom) {
        throw ProtocolError("zoom out of range");
    }
    return z;
}

int rotation(Reader& r) {
    const int rot = r.i32();
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
        throw ProtocolError("rotation is not 0, 90, 180 or 270");
    }
    return rot;
}

float coord(Reader& r) {
    const float c = r.f32();
    if (c < -kMaxCoord || c > kMaxCoord) {
        throw ProtocolError("coordinate out of range");
    }
    return c;
}

void put_quads(Writer& w, const std::vector<TextQuad>& quads) {
    w.u32(static_cast<std::uint32_t>(quads.size()));
    for (const TextQuad& q : quads) {
        for (float v : {q.ul_x, q.ul_y, q.ur_x, q.ur_y, q.ll_x, q.ll_y, q.lr_x, q.lr_y}) {
            w.f32(v);
        }
    }
}

std::vector<TextQuad> get_quads(Reader& r) {
    const std::size_t n = r.count(kQuadBytes);
    std::vector<TextQuad> quads;
    quads.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        TextQuad q;
        q.ul_x = coord(r); q.ul_y = coord(r);
        q.ur_x = coord(r); q.ur_y = coord(r);
        q.ll_x = coord(r); q.ll_y = coord(r);
        q.lr_x = coord(r); q.lr_y = coord(r);
        quads.push_back(q);
    }
    return quads;
}

}  // namespace

bool is_known(std::uint16_t type) noexcept {
    switch (static_cast<MsgType>(type)) {
    case MsgType::Hello: case MsgType::Open: case MsgType::Authenticate:
    case MsgType::Render: case MsgType::Cancel: case MsgType::Search:
    case MsgType::Select: case MsgType::Shutdown:
    case MsgType::HelloAck: case MsgType::NeedsPassword: case MsgType::Opened:
    case MsgType::Outline: case MsgType::Rendered: case MsgType::RenderSkipped:
    case MsgType::PageMatches: case MsgType::SearchDone:
    case MsgType::SelectionResult: case MsgType::Failed:
        return true;
    }
    return false;
}

void Hello::encode(Writer& w) const { w.u32(version); }
Hello Hello::decode(Reader& r) { return {r.u32()}; }

void Open::encode(Writer& w) const { w.str(name); }
Open Open::decode(Reader& r) {
    Open m;
    m.name = r.str(4096);
    if (m.name.find('/') != std::string::npos || m.name.find('\0') != std::string::npos) {
        throw ProtocolError("open hint must be a bare file name");
    }
    return m;
}

void Authenticate::encode(Writer& w) const { w.str(password); }
Authenticate Authenticate::decode(Reader& r) { return {r.str(kMaxString)}; }

void Render::encode(Writer& w) const {
    w.i32(page);
    w.f32(zoom);
    w.i32(rotation);
    w.u64(generation);
}
Render Render::decode(Reader& r) {
    Render m;
    m.page = page_index(r);
    m.zoom = ipc::zoom(r);
    m.rotation = ipc::rotation(r);
    m.generation = r.u64();
    return m;
}

void Cancel::encode(Writer& w) const { w.u64(generation); }
Cancel Cancel::decode(Reader& r) { return {r.u64()}; }

void Search::encode(Writer& w) const { w.str(needle); }
Search Search::decode(Reader& r) { return {r.str(kMaxString)}; }

void Select::encode(Writer& w) const {
    w.i32(page);
    w.f32(ax); w.f32(ay); w.f32(bx); w.f32(by);
    w.u8(static_cast<std::uint8_t>(mode));
}
Select Select::decode(Reader& r) {
    Select m;
    m.page = page_index(r);
    m.ax = coord(r); m.ay = coord(r); m.bx = coord(r); m.by = coord(r);
    const std::uint8_t mode = r.u8();
    if (mode > static_cast<std::uint8_t>(SelectMode::Lines)) {
        throw ProtocolError("unknown selection mode");
    }
    m.mode = static_cast<SelectMode>(mode);
    return m;
}

void HelloAck::encode(Writer& w) const { w.u32(version); }
HelloAck HelloAck::decode(Reader& r) { return {r.u32()}; }

void NeedsPassword::encode(Writer& w) const { w.u8(retry ? 1 : 0); }
NeedsPassword NeedsPassword::decode(Reader& r) { return {r.boolean()}; }

void Opened::encode(Writer& w) const {
    w.u32(static_cast<std::uint32_t>(base_sizes.size()));
    for (const PageSize& s : base_sizes) {
        w.i32(s.width);
        w.i32(s.height);
    }
}
Opened Opened::decode(Reader& r) {
    const std::size_t n = r.count(8);
    if (n > static_cast<std::size_t>(kMaxPage)) {
        throw ProtocolError("page count out of range");
    }
    Opened m;
    m.base_sizes.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        PageSize s{r.i32(), r.i32()};
        // Zero is legal: a page whose size could not be computed.
        if (s.width < 0 || s.height < 0 || s.width > kMaxEdge || s.height > kMaxEdge) {
            throw ProtocolError("page size out of range");
        }
        m.base_sizes.push_back(s);
    }
    return m;
}

void Outline::encode(Writer& w) const {
    w.u32(static_cast<std::uint32_t>(rows.size()));
    for (const OutlineRow& row : rows) {
        w.i32(row.depth);
        w.str(row.title);
        w.i32(row.page);
        w.f32(row.y);
    }
}
Outline Outline::decode(Reader& r) {
    const std::size_t n = r.count(16);
    Outline m;
    m.rows.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        OutlineRow row;
        row.depth = r.i32();
        if (row.depth < 0 || row.depth > kMaxDepth) {
            throw ProtocolError("outline depth out of range");
        }
        row.title = r.str(kMaxString);
        row.page = r.i32();
        if (row.page < -1 || row.page > kMaxPage) {
            throw ProtocolError("outline page out of range");
        }
        row.y = coord(r);
        m.rows.push_back(std::move(row));
    }
    return m;
}

void Rendered::encode(Writer& w) const {
    w.i32(page);
    w.f32(zoom);
    w.i32(rotation);
    w.u64(generation);
    w.i32(bitmap.width);
    w.i32(bitmap.height);
    w.i32(bitmap.stride);
    w.i32(bitmap.channels);
    w.bytes(bitmap.pixels);
}
Rendered Rendered::decode(Reader& r) {
    Rendered m;
    m.page = page_index(r);
    m.zoom = ipc::zoom(r);
    m.rotation = ipc::rotation(r);
    m.generation = r.u64();

    Bitmap& b = m.bitmap;
    b.width = r.i32();
    b.height = r.i32();
    b.stride = r.i32();
    b.channels = r.i32();
    if (b.channels != 3) {
        throw ProtocolError("bitmap is not RGB");
    }
    if (b.width <= 0 || b.height <= 0 || b.width > kMaxEdge || b.height > kMaxEdge) {
        throw ProtocolError("bitmap dimensions out of range");
    }
    // Widths are bounded by kMaxEdge, so none of these products can overflow
    // std::size_t -- but compute them in size_t anyway, never in int.
    const auto width = static_cast<std::size_t>(b.width);
    const auto height = static_cast<std::size_t>(b.height);
    const auto stride = static_cast<std::size_t>(b.stride);
    if (b.stride < 0 || stride < width * 3 || stride > width * 3 + 64) {
        throw ProtocolError("bitmap stride inconsistent with width");
    }
    b.pixels = r.bytes(kMaxPayload);
    if (b.pixels.size() != stride * height) {
        throw ProtocolError("bitmap pixel data does not match its dimensions");
    }
    return m;
}

void RenderSkipped::encode(Writer& w) const {
    w.i32(page);
    w.u64(generation);
}
RenderSkipped RenderSkipped::decode(Reader& r) {
    RenderSkipped m;
    m.page = page_index(r);
    m.generation = r.u64();
    return m;
}

void PageMatches::encode(Writer& w) const {
    w.i32(page);
    put_quads(w, quads);
}
PageMatches PageMatches::decode(Reader& r) {
    PageMatches m;
    m.page = page_index(r);
    m.quads = get_quads(r);
    return m;
}

void SearchDone::encode(Writer& w) const { w.u32(total); }
SearchDone SearchDone::decode(Reader& r) { return {r.u32()}; }

void SelectionResult::encode(Writer& w) const {
    w.i32(page);
    put_quads(w, quads);
    w.str(text);
}
SelectionResult SelectionResult::decode(Reader& r) {
    SelectionResult m;
    m.page = page_index(r);
    m.quads = get_quads(r);
    m.text = r.str(kMaxString);
    return m;
}

void Failed::encode(Writer& w) const { w.str(message); }
Failed Failed::decode(Reader& r) { return {r.str(kMaxString)}; }

}  // namespace leht::ipc
