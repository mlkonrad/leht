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

/// A 0-1 colour or opacity component.
float unit(Reader& r) {
    const float v = r.f32();
    if (!(v >= 0.0F && v <= 1.0F)) {
        throw ProtocolError("value outside 0-1");
    }
    return v;
}

/// A size or width in points: positive and sane.
float points(Reader& r) {
    const float v = r.f32();
    if (!(v >= 0.0F && v <= 10000.0F)) {
        throw ProtocolError("size out of range");
    }
    return v;
}

void put_rect(Writer& w, const Rect& rect) {
    w.f32(rect.x0); w.f32(rect.y0); w.f32(rect.x1); w.f32(rect.y1);
}

Rect get_rect(Reader& r) {
    Rect rect;
    rect.x0 = coord(r); rect.y0 = coord(r); rect.x1 = coord(r); rect.y1 = coord(r);
    return rect;
}

void put_sizes(Writer& w, const std::vector<PageSize>& sizes) {
    w.u32(static_cast<std::uint32_t>(sizes.size()));
    for (const PageSize& s : sizes) {
        w.i32(s.width);
        w.i32(s.height);
    }
}

std::vector<PageSize> get_sizes(Reader& r) {
    const std::size_t n = r.count(8);
    if (n > static_cast<std::size_t>(kMaxPage)) {
        throw ProtocolError("page count out of range");
    }
    std::vector<PageSize> sizes;
    sizes.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        PageSize s{r.i32(), r.i32()};
        // Zero is legal: a page whose size could not be computed.
        if (s.width < 0 || s.height < 0 || s.width > kMaxEdge || s.height > kMaxEdge) {
            throw ProtocolError("page size out of range");
        }
        sizes.push_back(s);
    }
    return sizes;
}

void put_strings(Writer& w, const std::vector<std::string>& items) {
    w.u32(static_cast<std::uint32_t>(items.size()));
    for (const std::string& item : items) {
        w.str(item);
    }
}

std::vector<std::string> get_strings(Reader& r, std::size_t max_len) {
    const std::size_t n = r.count(4);
    std::vector<std::string> items;
    items.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        items.push_back(r.str(max_len));
    }
    return items;
}

constexpr std::size_t kMaxName = 4096;  ///< field names, annotation types, stamp names

}  // namespace

bool takes_fd(MsgType type) noexcept {
    return type == MsgType::Open || type == MsgType::Save;
}

bool is_known(std::uint16_t type) noexcept {
    switch (static_cast<MsgType>(type)) {
    case MsgType::Hello: case MsgType::Open: case MsgType::Authenticate:
    case MsgType::Render: case MsgType::Cancel: case MsgType::Search:
    case MsgType::Select: case MsgType::Shutdown: case MsgType::CancelSearch:
    case MsgType::HelloAck: case MsgType::NeedsPassword: case MsgType::Opened:
    case MsgType::Outline: case MsgType::Rendered: case MsgType::RenderSkipped:
    case MsgType::PageMatches: case MsgType::SearchDone:
    case MsgType::SelectionResult: case MsgType::Failed:
    case MsgType::Edit: case MsgType::Save: case MsgType::ListAnnots:
    case MsgType::ListFields: case MsgType::Edited: case MsgType::Saved:
    case MsgType::AnnotList: case MsgType::FieldList:
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

void Search::encode(Writer& w) const {
    w.str(needle);
    w.u64(epoch);
}
Search Search::decode(Reader& r) {
    Search m;
    m.needle = r.str(kMaxString);
    m.epoch = r.u64();
    return m;
}

void CancelSearch::encode(Writer& w) const { w.u64(epoch); }
CancelSearch CancelSearch::decode(Reader& r) { return {r.u64()}; }

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

void Opened::encode(Writer& w) const { put_sizes(w, base_sizes); }
Opened Opened::decode(Reader& r) { return {get_sizes(r)}; }

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

// --- editing -------------------------------------------------------------------

void Edit::encode(Writer& w) const {
    w.u8(static_cast<std::uint8_t>(kind));
    switch (kind) {
    case Kind::Redact:
        w.i32(page);
        w.u32(static_cast<std::uint32_t>(rects.size()));
        for (const Rect& rect : rects) {
            put_rect(w, rect);
        }
        break;
    case Kind::RedactText:
        w.str(text);
        break;
    case Kind::AddAnnot: {
        const ops::AnnotSpec& a = annot;
        w.i32(page);
        w.u8(static_cast<std::uint8_t>(a.kind));
        put_quads(w, a.quads);
        put_rect(w, a.rect);
        w.u32(static_cast<std::uint32_t>(a.strokes.size()));
        for (const auto& stroke : a.strokes) {
            w.u32(static_cast<std::uint32_t>(stroke.size()));
            for (const Point& p : stroke) {
                w.f32(p.x);
                w.f32(p.y);
            }
        }
        w.str(a.contents);
        w.str(a.author);
        for (const float c : a.color) {
            w.f32(c);
        }
        w.f32(a.opacity);
        w.f32(a.font_size);
        w.f32(a.line_width);
        w.str(a.stamp);
        break;
    }
    case Kind::DeleteAnnot:
        w.i32(annot_id);
        break;
    case Kind::SetField:
        w.str(name);
        w.str(text);
        break;
    case Kind::Watermark: {
        const ops::WatermarkOptions& o = watermark;
        w.str(pages);
        w.str(o.text);
        w.f32(o.font_size);
        w.f32(o.opacity);
        w.f32(o.angle);
        for (const float c : o.color) {
            w.f32(c);
        }
        w.u8(o.under ? 1 : 0);
        break;
    }
    case Kind::CropMargins:
        w.str(pages);
        w.f32(margins.left); w.f32(margins.top); w.f32(margins.right); w.f32(margins.bottom);
        break;
    }
}

Edit Edit::decode(Reader& r) {
    Edit m;
    const std::uint8_t kind = r.u8();
    if (kind < 1 || kind > static_cast<std::uint8_t>(Kind::CropMargins)) {
        throw ProtocolError("unknown edit kind");
    }
    m.kind = static_cast<Kind>(kind);
    switch (m.kind) {
    case Kind::Redact: {
        m.page = page_index(r);
        const std::size_t n = r.count(16);
        m.rects.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            m.rects.push_back(get_rect(r));
        }
        break;
    }
    case Kind::RedactText:
        m.text = r.str(kMaxString);
        break;
    case Kind::AddAnnot: {
        ops::AnnotSpec& a = m.annot;
        m.page = page_index(r);
        const std::uint8_t ak = r.u8();
        if (ak > static_cast<std::uint8_t>(ops::AnnotKind::Stamp)) {
            throw ProtocolError("unknown annotation kind");
        }
        a.kind = static_cast<ops::AnnotKind>(ak);
        a.quads = get_quads(r);
        a.rect = get_rect(r);
        const std::size_t strokes = r.count(4);
        a.strokes.reserve(strokes);
        for (std::size_t i = 0; i < strokes; ++i) {
            const std::size_t points_n = r.count(8);
            std::vector<Point> stroke;
            stroke.reserve(points_n);
            for (std::size_t k = 0; k < points_n; ++k) {
                const float x = coord(r);
                stroke.push_back({x, coord(r)});
            }
            a.strokes.push_back(std::move(stroke));
        }
        a.contents = r.str(kMaxString);
        a.author = r.str(kMaxName);
        for (float& c : a.color) {
            c = unit(r);
        }
        a.opacity = unit(r);
        a.font_size = points(r);
        a.line_width = points(r);
        a.stamp = r.str(kMaxName);
        break;
    }
    case Kind::DeleteAnnot:
        m.annot_id = r.i32();
        break;
    case Kind::SetField:
        m.name = r.str(kMaxName);
        m.text = r.str(kMaxString);
        break;
    case Kind::Watermark: {
        ops::WatermarkOptions& o = m.watermark;
        m.pages = r.str(kMaxName);
        o.text = r.str(kMaxName);
        o.font_size = points(r);
        o.opacity = unit(r);
        o.angle = r.f32();
        for (float& c : o.color) {
            c = unit(r);
        }
        o.under = r.boolean();
        break;
    }
    case Kind::CropMargins:
        m.pages = r.str(kMaxName);
        m.margins.left = points(r);
        m.margins.top = points(r);
        m.margins.right = points(r);
        m.margins.bottom = points(r);
        break;
    }
    return m;
}

void Edited::encode(Writer& w) const {
    w.u8(all_pages ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(pages.size()));
    for (const int p : pages) {
        w.i32(p);
    }
    put_sizes(w, base_sizes);
    w.i32(annot_id);
    put_strings(w, remaining);
}
Edited Edited::decode(Reader& r) {
    Edited m;
    m.all_pages = r.boolean();
    const std::size_t n = r.count(4);
    m.pages.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        m.pages.push_back(page_index(r));
    }
    m.base_sizes = get_sizes(r);
    m.annot_id = r.i32();
    if (m.annot_id < 0) {
        throw ProtocolError("annotation id out of range");
    }
    m.remaining = get_strings(r, kMaxString);
    return m;
}

void Saved::encode(Writer& w) const { w.u64(bytes); }
Saved Saved::decode(Reader& r) { return {r.u64()}; }

void AnnotList::encode(Writer& w) const {
    w.u32(static_cast<std::uint32_t>(items.size()));
    for (const ops::AnnotInfo& a : items) {
        w.i32(a.id);
        w.i32(a.page);
        w.str(a.type);
        put_rect(w, a.rect);
        w.str(a.contents);
        w.str(a.author);
    }
}
AnnotList AnnotList::decode(Reader& r) {
    const std::size_t n = r.count(36);
    AnnotList m;
    m.items.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        ops::AnnotInfo a;
        a.id = r.i32();
        if (a.id <= 0) {
            throw ProtocolError("annotation id out of range");
        }
        a.page = page_index(r);
        a.type = r.str(kMaxName);
        a.rect = get_rect(r);
        a.contents = r.str(kMaxString);
        a.author = r.str(kMaxString);
        m.items.push_back(std::move(a));
    }
    return m;
}

void FieldList::encode(Writer& w) const {
    w.u32(static_cast<std::uint32_t>(items.size()));
    for (const ops::FieldInfo& f : items) {
        w.str(f.name);
        w.u8(static_cast<std::uint8_t>(f.type));
        w.str(f.value);
        put_strings(w, f.options);
        w.i32(f.page);
        put_rect(w, f.rect);
        w.u8(f.read_only ? 1 : 0);
        w.u8(f.required ? 1 : 0);
        w.i32(f.max_length);
    }
}
FieldList FieldList::decode(Reader& r) {
    const std::size_t n = r.count(35);
    FieldList m;
    m.items.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        ops::FieldInfo f;
        f.name = r.str(kMaxString);
        const std::uint8_t type = r.u8();
        if (type > static_cast<std::uint8_t>(ops::FieldType::Unknown)) {
            throw ProtocolError("unknown field type");
        }
        f.type = static_cast<ops::FieldType>(type);
        f.value = r.str(kMaxString);
        f.options = get_strings(r, kMaxString);
        f.page = page_index(r);
        f.rect = get_rect(r);
        f.read_only = r.boolean();
        f.required = r.boolean();
        f.max_length = r.i32();
        if (f.max_length < 0) {
            throw ProtocolError("field length limit out of range");
        }
        m.items.push_back(std::move(f));
    }
    return m;
}

}  // namespace leht::ipc
