// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/ipc/protocol.hpp"
#include "leht/ipc/wire.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace leht::ipc {

/// Raised by Channel::recv when a whole frame did not arrive in time.
class Timeout : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Owns a file descriptor and closes it on destruction.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept;
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd();

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

/// One framed message. On the wire:
///
///     u32 payload_length | u16 type | u64 id | payload
///
/// all little-endian. `id` pairs a response with its request; the worker
/// echoes it. A frame may carry one file descriptor (only Open does).
struct Frame {
    MsgType type{};
    std::uint64_t id = 0;
    std::vector<std::uint8_t> payload;
    UniqueFd fd;
};

inline constexpr std::size_t kHeaderBytes = 4 + 2 + 8;

/// Builds a frame from a message struct.
template <typename Msg>
Frame make_frame(std::uint64_t id, const Msg& msg) {
    Writer w;
    msg.encode(w);
    Frame f;
    f.type = Msg::kType;
    f.id = id;
    f.payload = std::move(w.buffer());
    return f;
}

/// Decodes a frame's payload as `Msg`. Throws ProtocolError if the frame is of
/// another type, or the payload is malformed or has trailing bytes.
template <typename Msg>
Msg decode_as(const Frame& f) {
    if (f.type != Msg::kType) {
        throw ProtocolError("unexpected message type");
    }
    Reader r(f.payload);
    Msg m = Msg::decode(r);
    r.finish();
    return m;
}

/// A connected stream socket speaking framed messages.
///
/// send() may be called from several threads (it is serialised internally).
/// recv() must only ever be called from one thread at a time. One sending
/// thread and one receiving thread may run concurrently.
class Channel {
public:
    /// `accept_fds` controls whether an incoming frame may carry a file
    /// descriptor. The viewer passes false: a worker has no business sending
    /// it descriptors, and one that does is treated as hostile.
    explicit Channel(UniqueFd socket, bool accept_fds);

    /// Sends a frame. If `pass_fd` is >= 0 it is attached via SCM_RIGHTS; the
    /// caller keeps ownership of its copy. Throws std::system_error on I/O
    /// failure (including a peer that has gone away).
    void send(const Frame& frame, int pass_fd = -1);

    template <typename Msg>
    void send(std::uint64_t id, const Msg& msg, int pass_fd = -1) {
        send(make_frame(id, msg), pass_fd);
    }

    /// Receives the next frame. Returns nullopt on a clean EOF between frames.
    /// Throws ProtocolError on a malformed header, an unknown type, an
    /// oversize payload, an unwanted fd, or EOF mid-frame; std::system_error on
    /// I/O failure.
    std::optional<Frame> recv();

    /// As recv(), but throws Timeout unless the whole frame -- header and
    /// payload -- arrives within `timeout`. A deadline on the frame rather
    /// than on each read, so a peer trickling bytes cannot extend it.
    std::optional<Frame> recv(std::chrono::milliseconds timeout);

    /// Shuts the socket down in both directions, waking a thread blocked in
    /// recv(). Safe to call from any thread.
    void shutdown() noexcept;

    [[nodiscard]] int fd() const noexcept { return sock_.get(); }

private:
    /// Reads exactly `n` bytes into `dst`, collecting any passed fd into
    /// `got_fd`. Returns false on EOF before the first byte when `eof_ok`.
    using Deadline = std::optional<std::chrono::steady_clock::time_point>;

    bool read_exact(std::uint8_t* dst, std::size_t n, UniqueFd& got_fd, bool eof_ok,
                    Deadline deadline);
    std::optional<Frame> recv_until(Deadline deadline);

    UniqueFd sock_;
    bool accept_fds_;
    std::mutex send_mutex_;
};

/// A connected pair of stream sockets, both close-on-exec.
std::pair<UniqueFd, UniqueFd> socket_pair();

}  // namespace leht::ipc
