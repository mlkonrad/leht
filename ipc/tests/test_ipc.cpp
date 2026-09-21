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
        // Even where fds are allowed, only Open may carry one.
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
