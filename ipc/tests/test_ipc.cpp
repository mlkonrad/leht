// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The wire format between the viewer and leht-worker. Round-trips prove the
// encoding; the rest proves the decoder's real job, which is refusing whatever
// a compromised worker might write.

#include "test_harness.hpp"

#include "leht/ipc/channel.hpp"
#include "leht/ipc/protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

using namespace leht::ipc;

namespace {

template <typename Msg>
Msg round_trip(const Msg& in) {
    return decode_as<Msg>(make_frame(7, in));
}

template <typename Msg>
bool rejects(const std::vector<std::uint8_t>& payload) {
    Frame f;
    f.type = Msg::kType;
    f.payload = payload;
    try {
        (void)decode_as<Msg>(f);
    } catch (const ProtocolError&) {
        return true;
    }
    return false;
}

leht::Bitmap small_bitmap() {
    leht::Bitmap b;
    b.width = 3;
    b.height = 2;
    b.channels = 3;
    b.stride = 9;
    for (std::uint8_t i = 0; i < 18; ++i) {
        b.pixels.push_back(i);
    }
    return b;
}

Rendered sample_rendered() {
    Rendered m;
    m.page = 4;
    m.zoom = 1.5F;
    m.rotation = 90;
    m.generation = 1234567890123ULL;
    m.bitmap = small_bitmap();
    return m;
}

std::vector<std::uint8_t> encode_rendered(const leht::Bitmap& b) {
    Rendered m = sample_rendered();
    m.bitmap = b;
    return make_frame(1, m).payload;
}

void test_round_trips() {
    CHECK(round_trip(Hello{}).version == kProtocolVersion);
    CHECK(round_trip(Open{"report.pdf"}).name == "report.pdf");
    CHECK(rejects<Open>(make_frame(1, Open{"../etc/passwd"}).payload));
    CHECK(round_trip(Authenticate{"s3cr\xc3\xa9t"}).password == "s3cr\xc3\xa9t");
    CHECK(round_trip(Cancel{42}).generation == 42);
    CHECK(round_trip(CancelSearch{9}).epoch == 9);
    const Search sr0 = round_trip(Search{"n", 4});
    CHECK(sr0.needle == "n" && sr0.epoch == 4);
    CHECK(round_trip(Search{"needle"}).needle == "needle");
    CHECK(round_trip(NeedsPassword{true}).retry);
    CHECK(round_trip(SearchDone{17}).total == 17);
    CHECK(round_trip(Failed{"nope"}).message == "nope");

    const Render r = round_trip(Render{3, 2.25F, 270, 99});
    CHECK(r.page == 3 && r.zoom == 2.25F && r.rotation == 270 && r.generation == 99);

    Select s;
    s.page = 1;
    s.ax = 1.5F; s.ay = -2.0F; s.bx = 300.0F; s.by = 400.25F;
    s.mode = leht::SelectMode::Words;
    const Select s2 = round_trip(s);
    CHECK(s2.page == 1 && s2.ax == 1.5F && s2.ay == -2.0F && s2.bx == 300.0F &&
          s2.by == 400.25F && s2.mode == leht::SelectMode::Words);

    Opened o;
    o.base_sizes = {{612, 792}, {0, 0}, {842, 595}};
    const Opened o2 = round_trip(o);
    CHECK(o2.base_sizes.size() == 3 && o2.base_sizes[2].width == 842 &&
          o2.base_sizes[1].height == 0);

    Outline ol;
    ol.rows = {{0, "Intro", 0, 10.0F}, {1, "Detail", 5, 0.0F}, {0, "No target", -1, 0.0F}};
    const Outline ol2 = round_trip(ol);
    CHECK(ol2.rows.size() == 3 && ol2.rows[1].title == "Detail" && ol2.rows[1].depth == 1 &&
          ol2.rows[2].page == -1);

    const Rendered rd = round_trip(sample_rendered());
    CHECK(rd.page == 4 && rd.rotation == 90 && rd.generation == 1234567890123ULL);
    CHECK(rd.bitmap.width == 3 && rd.bitmap.height == 2 && rd.bitmap.stride == 9);
    CHECK(rd.bitmap.pixels == small_bitmap().pixels);

    leht::TextQuad q;
    q.ul_x = 1; q.ul_y = 2; q.ur_x = 3; q.ur_y = 4;
    q.ll_x = 5; q.ll_y = 6; q.lr_x = 7; q.lr_y = 8;
    const PageMatches pm = round_trip(PageMatches{2, {q, q}});
    CHECK(pm.page == 2 && pm.quads.size() == 2 && pm.quads[1].lr_y == 8.0F);

    const SelectionResult sr = round_trip(SelectionResult{0, {q}, "hello"});
    CHECK(sr.text == "hello" && sr.quads.size() == 1 && sr.quads[0].ur_x == 3.0F);

    const RenderSkipped rs = round_trip(RenderSkipped{8, 5});
    CHECK(rs.page == 8 && rs.generation == 5);
}

Edit sample_annot_edit() {
    Edit e;
    e.kind = Edit::Kind::AddAnnot;
    e.page = 3;
    e.annot.kind = leht::ops::AnnotKind::Ink;
    e.annot.rect = {1, 2, 3, 4};
    e.annot.strokes = {{{1, 2}, {3, 4}}, {{5, 6}}};
    e.annot.contents = "note";
    e.annot.author = "me";
    e.annot.color[1] = 0.5F;
    e.annot.opacity = 0.75F;
    e.annot.stamp = "Approved";
    return e;
}

Edited sample_edited() {
    Edited e;
    e.pages = {0, 4};
    e.base_sizes = {{612, 792}, {300, 400}};
    e.annot_id = 42;
    e.remaining = {"document metadata (Title)"};
    return e;
}

FieldList sample_fields() {
    leht::ops::FieldInfo f;
    f.name = "address.street";
    f.type = leht::ops::FieldType::Choice;
    f.value = "fi";
    f.options = {"Estonia", "Finland"};
    f.page = 2;
    f.rect = {10, 20, 30, 40};
    f.read_only = true;
    f.max_length = 12;
    return FieldList{{f}};
}

void test_edit_messages_round_trip() {
    Edit redact;
    redact.kind = Edit::Kind::Redact;
    redact.page = 1;
    redact.rects = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    const Edit r2 = round_trip(redact);
    CHECK(r2.kind == Edit::Kind::Redact && r2.page == 1 && r2.rects.size() == 2 &&
          r2.rects[1].y1 == 8.0F);

    const Edit a = round_trip(sample_annot_edit());
    CHECK(a.kind == Edit::Kind::AddAnnot && a.page == 3);
    CHECK(a.annot.kind == leht::ops::AnnotKind::Ink && a.annot.strokes.size() == 2 &&
          a.annot.strokes[0][1].y == 4.0F && a.annot.contents == "note" &&
          a.annot.author == "me" && a.annot.color[1] == 0.5F && a.annot.opacity == 0.75F &&
          a.annot.stamp == "Approved");

    Edit field;
    field.kind = Edit::Kind::SetField;
    field.name = "name";
    field.text = "Marlon";
    const Edit f2 = round_trip(field);
    CHECK(f2.name == "name" && f2.text == "Marlon");

    Edit wm;
    wm.kind = Edit::Kind::Watermark;
    wm.pages = "1-3";
    wm.watermark.text = "DRAFT";
    wm.watermark.angle = -30;
    wm.watermark.under = true;
    const Edit w2 = round_trip(wm);
    CHECK(w2.pages == "1-3" && w2.watermark.text == "DRAFT" && w2.watermark.angle == -30.0F &&
          w2.watermark.under);

    Edit crop;
    crop.kind = Edit::Kind::CropMargins;
    crop.margins = {1, 2, 3, 4};
    CHECK(round_trip(crop).margins.bottom == 4.0F);

    Edit move;
    move.kind = Edit::Kind::MoveAnnot;
    move.annot_id = 9;
    move.rects = {{10, 20, 30, 40}};
    const Edit m2 = round_trip(move);
    CHECK(m2.annot_id == 9 && m2.rects.size() == 1 && m2.rects[0].y1 == 40.0F);

    Edit retext;
    retext.kind = Edit::Kind::SetAnnotContents;
    retext.annot_id = 11;
    retext.text = "new words";
    const Edit t2 = round_trip(retext);
    CHECK(t2.annot_id == 11 && t2.text == "new words");

    Edit box;
    box.kind = Edit::Kind::CropBox;
    box.pages = "2-";
    box.rects = {{5, 6, 500, 700}};
    const Edit b2 = round_trip(box);
    CHECK(b2.pages == "2-" && b2.rects.size() == 1 && b2.rects[0].x1 == 500.0F);

    Edit layer;
    layer.kind = Edit::Kind::AddTextLayer;
    layer.page = 3;
    layer.words = {{"Tere", {1, 2, 30, 14}}, {"õhtust", {34, 2, 80, 14}}};
    const Edit l2 = round_trip(layer);
    CHECK(l2.page == 3 && l2.words.size() == 2 && l2.words[1].text == "õhtust" &&
          l2.words[1].box.x1 == 80.0F);

    Recognize rec;
    rec.zoom = 300.0F / 72.0F;
    rec.bitmap.width = 4;
    rec.bitmap.height = 2;
    rec.bitmap.stride = 12;
    rec.bitmap.channels = 3;
    rec.bitmap.pixels.assign(24, 200);
    const Recognize rec2 = round_trip(rec);
    CHECK(rec2.bitmap.width == 4 && rec2.bitmap.pixels.size() == 24 && rec2.zoom > 4.1F);
    const TextPageList tp = round_trip(TextPageList{{0, 4, 7}});
    CHECK(tp.pages == std::vector<int>({0, 4, 7}));
    const Words words = round_trip(Words{layer.words});
    CHECK(words.words.size() == 2 && words.words[0].text == "Tere");

    Edit del;
    del.kind = Edit::Kind::DeleteAnnot;
    del.annot_id = 17;
    CHECK(round_trip(del).annot_id == 17);

    Edit text;
    text.kind = Edit::Kind::RedactText;
    text.text = "secret";
    CHECK(round_trip(text).text == "secret");

    const Edited e = round_trip(sample_edited());
    CHECK(!e.all_pages && e.pages == std::vector<int>({0, 4}) && e.base_sizes.size() == 2 &&
          e.base_sizes[1].height == 400 && e.annot_id == 42 && e.remaining.size() == 1);

    CHECK(round_trip(Saved{123456}).bytes == 123456);

    leht::ops::AnnotInfo info{7, 1, "FreeText", {1, 2, 3, 4}, "c", "a"};
    info.movable = true;
    info.resizable = true;
    info.font_size = 14;
    info.color[0] = 0.5F;
    const AnnotList al = round_trip(AnnotList{{info}});
    CHECK(al.items.size() == 1 && al.items[0].id == 7 && al.items[0].type == "FreeText" &&
          al.items[0].rect.x1 == 3.0F && al.items[0].author == "a" && al.items[0].movable &&
          al.items[0].resizable && al.items[0].font_size == 14.0F &&
          al.items[0].color[0] == 0.5F);

    const FieldList fl = round_trip(sample_fields());
    CHECK(fl.items.size() == 1 && fl.items[0].name == "address.street" &&
          fl.items[0].type == leht::ops::FieldType::Choice && fl.items[0].options.size() == 2 &&
          fl.items[0].read_only && fl.items[0].max_length == 12);
}

/// Every prefix of `msg`'s payload, and one trailing byte, must be refused.
template <typename Msg>
void check_truncations(const Msg& msg) {
    const auto full = make_frame(1, msg).payload;
    for (std::size_t n = 0; n < full.size(); ++n) {
        const std::vector<std::uint8_t> cut(full.begin(),
                                            full.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(rejects<Msg>(cut));
    }
    auto trailing = full;
    trailing.push_back(0);
    CHECK(rejects<Msg>(trailing));
}

PrepareSignature sample_prepare() {
    PrepareSignature m;
    m.request.field = "Approver";
    m.request.page = 2;
    m.request.rect = {10, 20, 110, 70};
    m.request.name = "Mari Maasikas";
    m.request.reason = "Approved";
    m.request.location = "Tallinn";
    m.request.time = 1'790'000'000;
    m.request.reserve = 8192;
    m.request.appearance.image = {0x89, 'P', 'N', 'G'};
    m.request.appearance.strokes = {{{1, 2}, {3, 4}}, {{5, 6}}};
    m.request.appearance.strokes_width = 100;
    m.request.appearance.strokes_height = 40;
    m.request.appearance.stroke_width = 1.5F;
    m.request.appearance.lines = {"Mari Maasikas", "2026-09-22"};
    return m;
}

SignatureList sample_signature_list() {
    CertRow signer;
    signer.subject = "CN=Mari Maasikas,C=EE";
    signer.common_name = "Mari Maasikas";
    signer.issuer = "CN=Test Root";
    signer.serial = "0a0b";
    signer.sha256 = "abcdef";
    signer.not_before = 1'700'000'000;
    signer.not_after = 1'800'000'000;
    signer.can_sign = true;

    SignatureRow row;
    row.field = "Signature1";
    row.page = 0;
    row.rect = {1, 2, 3, 4};
    row.subfilter = "ETSI.CAdES.detached";
    row.name = "Mari Maasikas";
    row.claimed_time = "D:20260922121500Z";
    row.range_ok = true;
    row.changed_after_signing = true;
    row.later_signature_covers_changes = true;
    row.covers_whole_revision = true;
    row.checked = true;
    row.intact = true;
    row.digest = "SHA-256";
    row.signer = signer;
    row.chain = {signer, signer};
    row.trust = 1;
    row.trust_detail = "self-signed certificate";
    row.has_signing_certificate_v2 = true;
    row.has_timestamp = true;
    row.timestamp_valid = true;
    row.timestamp_time = 1'790'000'001;
    row.authority = signer;
    SignatureList list;
    list.rows = {row};
    return list;
}

void test_signature_messages_round_trip() {
    const PrepareSignature p = round_trip(sample_prepare());
    CHECK(p.request.field == "Approver" && p.request.page == 2);
    CHECK(p.request.rect.x1 == 110.0F && p.request.reserve == 8192);
    CHECK(p.request.time == 1'790'000'000);
    CHECK(p.request.name == "Mari Maasikas" && p.request.reason == "Approved");
    CHECK(p.request.appearance.image.size() == 4 && p.request.appearance.image[0] == 0x89);
    CHECK(p.request.appearance.strokes.size() == 2 &&
          p.request.appearance.strokes[0][1].y == 4.0F &&
          p.request.appearance.strokes[1].size() == 1);
    CHECK(p.request.appearance.lines.size() == 2);

    CHECK(round_trip(ListSignatures{"-----BEGIN CERTIFICATE-----"}).trust_pem ==
          "-----BEGIN CERTIFICATE-----");

    SignaturePrepared prepared;
    prepared.range.v = {0, 1234, 5678, 90};
    prepared.field = "Signature1";
    const SignaturePrepared p2 = round_trip(prepared);
    CHECK(p2.range.v[1] == 1234 && p2.range.end() == 5768 && p2.field == "Signature1");

    const SignatureList l = round_trip(sample_signature_list());
    CHECK(l.rows.size() == 1);
    CHECK(l.rows[0].field == "Signature1" && l.rows[0].intact && l.rows[0].trust == 1);
    CHECK(l.rows[0].chain.size() == 2);
    CHECK(l.rows[0].signer.not_after == 1'800'000'000);
    CHECK(l.rows[0].has_timestamp && l.rows[0].timestamp_time == 1'790'000'001);
    CHECK(l.rows[0].later_signature_covers_changes);
}

void test_signature_messages_reject_hostile_input() {
    // A reserve nobody would ask for: a worker must not be talked into
    // allocating a megabyte-plus hole, nor a hole too small to be a signature.
    for (const std::uint64_t reserve : {std::uint64_t{0}, std::uint64_t{1023},
                                        std::uint64_t{1} << 40}) {
        PrepareSignature m = sample_prepare();
        m.request.reserve = static_cast<std::size_t>(reserve);
        CHECK(rejects<PrepareSignature>(make_frame(1, m).payload));
    }
    // A negative page, and a byte range with a negative span.
    {
        PrepareSignature m = sample_prepare();
        m.request.page = -1;
        CHECK(rejects<PrepareSignature>(make_frame(1, m).payload));
    }
    {
        SignaturePrepared m;
        m.range.v = {0, -1, 10, 10};
        CHECK(rejects<SignaturePrepared>(make_frame(1, m).payload));
    }
    check_truncations(sample_prepare());
    check_truncations(sample_signature_list());
    check_truncations(SignaturePrepared{});
}


void test_edit_messages_reject_hostile_input() {
    // What the worker sends back is what a compromised worker controls.
    check_truncations(sample_edited());
    check_truncations(sample_fields());
    check_truncations(AnnotList{{leht::ops::AnnotInfo{7, 1, "Ink", {}, "", ""}}});
    {
        Edit move;
        move.kind = Edit::Kind::MoveAnnot;
        move.annot_id = 9;
        move.rects = {{10, 20, 30, 40}};
        check_truncations(move);
        Edit retext;
        retext.kind = Edit::Kind::SetAnnotContents;
        retext.annot_id = 11;
        retext.text = "new words";
        check_truncations(retext);
        Edit box;
        box.kind = Edit::Kind::CropBox;
        box.rects = {{5, 6, 500, 700}};
        check_truncations(box);
        Edit layer;
        layer.kind = Edit::Kind::AddTextLayer;
        layer.words = {{"Tere", {1, 2, 30, 14}}};
        check_truncations(layer);
        check_truncations(Words{layer.words});
        Recognize rec;
        rec.bitmap.width = 2;
        rec.bitmap.height = 1;
        rec.bitmap.stride = 6;
        rec.bitmap.channels = 3;
        rec.bitmap.pixels.assign(6, 1);
        check_truncations(rec);
    }
    check_truncations(sample_annot_edit());

    Edited neg = sample_edited();
    neg.annot_id = -1;
    CHECK(rejects<Edited>(make_frame(1, neg).payload));
    Edited bad_page = sample_edited();
    bad_page.pages = {-3};
    CHECK(rejects<Edited>(make_frame(1, bad_page).payload));
    Edited huge = sample_edited();
    huge.base_sizes = {{100000, 10}};
    CHECK(rejects<Edited>(make_frame(1, huge).payload));

    CHECK(rejects<AnnotList>(make_frame(1, AnnotList{{leht::ops::AnnotInfo{0, 1, "X", {}, "", ""}}})
                                 .payload));

    FieldList bad_type = sample_fields();
    auto payload = make_frame(1, bad_type).payload;
    // The type byte follows the u32 count and the name (u32 length + bytes).
    const std::size_t type_at = 4 + 4 + bad_type.items[0].name.size();
    payload[type_at] = 99;
    CHECK(rejects<FieldList>(payload));

    Edit bad_kind = sample_annot_edit();
    auto ep = make_frame(1, bad_kind).payload;
    ep[0] = 42;
    CHECK(rejects<Edit>(ep));
    Edit bad_colour = sample_annot_edit();
    bad_colour.annot.color[0] = 2.0F;
    CHECK(rejects<Edit>(make_frame(1, bad_colour).payload));
}

void test_every_truncation_is_rejected() {
    const auto full = make_frame(1, sample_rendered()).payload;
    for (std::size_t n = 0; n < full.size(); ++n) {
        const std::vector<std::uint8_t> cut(full.begin(),
                                            full.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(rejects<Rendered>(cut));
    }
    auto trailing = full;
    trailing.push_back(0);
    CHECK(rejects<Rendered>(trailing));
}

void test_lying_bitmaps_are_rejected() {
    leht::Bitmap b = small_bitmap();

    b.height = 3;  // claims more rows than it carries
    CHECK(rejects<Rendered>(encode_rendered(b)));

    b = small_bitmap();
    b.stride = 6;  // stride narrower than width * 3
    CHECK(rejects<Rendered>(encode_rendered(b)));

    b = small_bitmap();
    b.channels = 4;
    b.stride = 12;
    b.pixels.resize(24);
    CHECK(rejects<Rendered>(encode_rendered(b)));

    b = small_bitmap();
    b.width = -3;
    CHECK(rejects<Rendered>(encode_rendered(b)));

    b = small_bitmap();
    b.width = std::numeric_limits<int>::max();
    b.stride = std::numeric_limits<int>::max();
    CHECK(rejects<Rendered>(encode_rendered(b)));
}

void test_semantic_limits() {
    CHECK(rejects<Render>(make_frame(1, Render{-1, 1.0F, 0, 0}).payload));
    CHECK(rejects<Render>(make_frame(1, Render{0, 0.0F, 0, 0}).payload));
    CHECK(rejects<Render>(make_frame(1, Render{0, 1000.0F, 0, 0}).payload));
    CHECK(rejects<Render>(make_frame(1, Render{0, 1.0F, 45, 0}).payload));
    CHECK(rejects<Render>(make_frame(1, Render{0, std::numeric_limits<float>::quiet_NaN(),
                                               0, 0}).payload));
    CHECK(rejects<NeedsPassword>({2}));

    Select s;
    auto p = make_frame(1, s).payload;
    p.back() = 9;  // unknown selection mode
    CHECK(rejects<Select>(p));

    // An element count the payload could not possibly hold must be refused
    // before anything is allocated for it.
    Writer w;
    w.u32(0xFFFFFFFFU);
    CHECK(rejects<Opened>(w.buffer()));
    CHECK(rejects<Outline>(w.buffer()));
    CHECK(rejects<Failed>(w.buffer()));

    Outline ol;
    ol.rows = {{-1, "x", 0, 0.0F}};
    CHECK(rejects<Outline>(make_frame(1, ol).payload));
}

void test_wrong_type_is_rejected() {
    bool threw = false;
    try {
        (void)decode_as<Render>(make_frame(1, Cancel{1}));
    } catch (const ProtocolError&) {
        threw = true;
    }
    CHECK(threw);
}

void write_all(int fd, const std::vector<std::uint8_t>& bytes) {
    CHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
}

std::vector<std::uint8_t> raw_header(std::uint32_t length, std::uint16_t type) {
    Writer w;
    w.u32(length);
    w.u16(type);
    w.u64(1);
    return w.buffer();
}

template <typename F>
bool recv_throws(F&& setup) {
    auto [a, b] = socket_pair();
    Channel rx(std::move(a), /*accept_fds=*/true);
    setup(b.get());
    b.reset();
    try {
        (void)rx.recv();
    } catch (const ProtocolError&) {
        return true;
    }
    return false;
}

void test_channel_round_trip_and_eof() {
    auto [a, b] = socket_pair();
    Channel left(std::move(a), false);
    Channel right(std::move(b), false);

    left.send(11, Search{"abc"});
    right.send(12, SearchDone{3});
    auto f1 = right.recv();
    auto f2 = left.recv();
    CHECK(f1 && f1->id == 11 && decode_as<Search>(*f1).needle == "abc");
    CHECK(f2 && f2->id == 12 && decode_as<SearchDone>(*f2).total == 3);

    left.shutdown();
    CHECK(!right.recv().has_value());  // clean EOF between frames
}

void test_channel_large_payload() {
    // Bigger than any socket buffer, so send() and recv() must both loop on
    // partial transfers.
    auto [a, b] = socket_pair();
    Channel tx(std::move(a), false);
    Channel rx(std::move(b), false);

    Rendered m = sample_rendered();
    m.bitmap.width = 2000;
    m.bitmap.height = 3000;
    m.bitmap.stride = 6000;
    m.bitmap.pixels.assign(6000U * 3000U, 0);
    for (std::size_t i = 0; i < m.bitmap.pixels.size(); i += 4099) {
        m.bitmap.pixels[i] = static_cast<std::uint8_t>(i);
    }

    std::thread sender([&] { tx.send(5, m); });
    auto f = rx.recv();
    sender.join();
    CHECK(f.has_value());
    const Rendered got = decode_as<Rendered>(*f);
    CHECK(got.bitmap.pixels == m.bitmap.pixels);
}

void test_hostile_headers() {
    // Unknown type.
    CHECK(recv_throws([](int fd) { write_all(fd, raw_header(0, 999)); }));
    // A length over the maximum is refused before anything is allocated.
    CHECK(recv_throws([](int fd) { write_all(fd, raw_header(0xFFFFFFFFU, 102)); }));
    // EOF inside the header.
    CHECK(recv_throws([](int fd) { write_all(fd, {1, 2, 3}); }));
    // EOF inside the payload.
    CHECK(recv_throws([](int fd) {
        auto h = raw_header(100, static_cast<std::uint16_t>(MsgType::Failed));
        h.push_back(0);
        write_all(fd, h);
    }));
}

void test_fd_passing() {
    int pipefd[2];
    CHECK(::pipe(pipefd) == 0);
    UniqueFd rd(pipefd[0]), wr(pipefd[1]);

    auto [a, b] = socket_pair();
    Channel tx(std::move(a), false);
    Channel rx(std::move(b), /*accept_fds=*/true);

    tx.send(1, Open{}, rd.get());
    auto f = rx.recv();
    CHECK(f && f->type == MsgType::Open && f->fd);

    // The received descriptor is a working copy of the pipe's read end.
    CHECK(::write(wr.get(), "x", 1) == 1);
    char c = 0;
    CHECK(::read(f->fd.get(), &c, 1) == 1 && c == 'x');
}

void test_unwanted_fds_are_rejected() {
    int pipefd[2];
    CHECK(::pipe(pipefd) == 0);
    UniqueFd rd(pipefd[0]), wr(pipefd[1]);

    {
        // The viewer never accepts descriptors from the worker.
        auto [a, b] = socket_pair();
        Channel tx(std::move(a), false);
        Channel rx(std::move(b), /*accept_fds=*/false);
        tx.send(1, Open{}, rd.get());
        bool threw = false;
        try {
            (void)rx.recv();
        } catch (const ProtocolError&) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        // Save carries one too: the output file.
        auto [a, b] = socket_pair();
        Channel tx(std::move(a), false);
        Channel rx(std::move(b), /*accept_fds=*/true);
        tx.send(1, Save{}, rd.get());
        auto f = rx.recv();
        CHECK(f && f->type == MsgType::Save && f->fd);
    }
    {
        // Even where fds are allowed, only Open and Save may carry one.
        auto [a, b] = socket_pair();
        Channel tx(std::move(a), false);
        Channel rx(std::move(b), /*accept_fds=*/true);
        tx.send(1, Search{"x"}, rd.get());
        bool threw = false;
        try {
            (void)rx.recv();
        } catch (const ProtocolError&) {
            threw = true;
        }
        CHECK(threw);
    }
}

void test_recv_timeout() {
    auto [a, b] = socket_pair();
    Channel rx(std::move(a), false);
    const int raw = b.get();

    // Nothing arrives: Timeout, promptly.
    const auto t0 = std::chrono::steady_clock::now();
    bool timed_out = false;
    try {
        (void)rx.recv(std::chrono::milliseconds(50));
    } catch (const Timeout&) {
        timed_out = true;
    }
    CHECK(timed_out);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));

    // A whole frame in time is received normally.
    Channel tx(std::move(b), false);
    tx.send(3, SearchDone{4});
    auto f = rx.recv(std::chrono::milliseconds(1000));
    CHECK(f && decode_as<SearchDone>(*f).total == 4);

    // Half a frame, then silence: the deadline covers the whole frame.
    const auto full = make_frame(9, Failed{"stalled"});
    Writer h;
    h.u32(static_cast<std::uint32_t>(full.payload.size()));
    h.u16(static_cast<std::uint16_t>(MsgType::Failed));
    h.u64(9);
    write_all(raw, h.buffer());
    timed_out = false;
    try {
        (void)rx.recv(std::chrono::milliseconds(50));
    } catch (const Timeout&) {
        timed_out = true;
    }
    CHECK(timed_out);
}

}  // namespace

int main() {
    RUN(test_round_trips);
    RUN(test_edit_messages_round_trip);
    RUN(test_edit_messages_reject_hostile_input);
    RUN(test_signature_messages_round_trip);
    RUN(test_signature_messages_reject_hostile_input);
    RUN(test_every_truncation_is_rejected);
    RUN(test_lying_bitmaps_are_rejected);
    RUN(test_semantic_limits);
    RUN(test_wrong_type_is_rejected);
    RUN(test_channel_round_trip_and_eof);
    RUN(test_channel_large_payload);
    RUN(test_hostile_headers);
    RUN(test_fd_passing);
    RUN(test_unwanted_fds_are_rejected);
    RUN(test_recv_timeout);
    return 0;
}
