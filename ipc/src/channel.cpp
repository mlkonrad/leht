// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ipc/channel.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace leht::ipc {

namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// The CMSG_* accessors are glibc C macros built from C-style casts and
// pointer arithmetic on socklen_t; they trip -Wold-style-cast, -Wcast-align and
// -Wsign-conversion inside the system header's expansion, not in anything we
// wrote. They are the only portable way to lay out SCM_RIGHTS control data, so
// they are confined to these two functions and nowhere else.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wsign-conversion"

constexpr std::size_t kCmsgSpace = CMSG_SPACE(sizeof(int));

void attach_fd(msghdr& msg, std::array<char, kCmsgSpace>& control, int fd) {
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
}

/// Takes ownership of every fd in the control data. Returns the first; any
/// extra (a peer that sent several) are closed and flagged via `extra`.
UniqueFd collect_fds(msghdr& msg, bool& extra) {
    UniqueFd first;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        const std::size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < n; ++i) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (!first) {
                first.reset(fd);
            } else {
                ::close(fd);
                extra = true;
            }
        }
    }
    return first;
}

#pragma GCC diagnostic pop

}  // namespace

UniqueFd& UniqueFd::operator=(UniqueFd&& o) noexcept {
    if (this != &o) {
        reset(o.release());
    }
    return *this;
}

UniqueFd::~UniqueFd() { reset(); }

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

std::pair<UniqueFd, UniqueFd> socket_pair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        throw_errno("socketpair");
    }
    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}

Channel::Channel(UniqueFd socket, bool accept_fds)
    : sock_(std::move(socket)), accept_fds_(accept_fds) {}

void Channel::send(const Frame& frame, int pass_fd) {
    if (frame.payload.size() > kMaxPayload) {
        throw ProtocolError("payload exceeds the protocol maximum");
    }
    Writer header;
    header.u32(static_cast<std::uint32_t>(frame.payload.size()));
    header.u16(static_cast<std::uint16_t>(frame.type));
    header.u64(frame.id);
    const auto& h = header.buffer();

    const std::scoped_lock lock(send_mutex_);

    // Two iovecs so the payload -- possibly a whole rendered page -- is never
    // copied just to prepend fourteen header bytes.
    std::array<iovec, 2> iov{};
    iov[0] = {const_cast<std::uint8_t*>(h.data()), h.size()};
    iov[1] = {const_cast<std::uint8_t*>(frame.payload.data()), frame.payload.size()};
    std::size_t first = 0;
    bool fd_pending = pass_fd >= 0;

    while (first < iov.size()) {
        msghdr msg{};
        msg.msg_iov = &iov[first];
        msg.msg_iovlen = iov.size() - first;
        alignas(cmsghdr) std::array<char, kCmsgSpace> control{};
        if (fd_pending) {
            attach_fd(msg, control, pass_fd);
        }
        const ssize_t n = ::sendmsg(sock_.get(), &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("sendmsg");
        }
        fd_pending = false;  // the kernel attached it to the first byte sent

        auto left = static_cast<std::size_t>(n);
        while (first < iov.size() && left >= iov[first].iov_len) {
            left -= iov[first].iov_len;
            ++first;
        }
        if (first < iov.size()) {
            iov[first].iov_base = static_cast<std::uint8_t*>(iov[first].iov_base) + left;
            iov[first].iov_len -= left;
        }
    }
}

bool Channel::read_exact(std::uint8_t* dst, std::size_t n, UniqueFd& got_fd,
                         bool eof_ok, Deadline deadline) {
    std::size_t done = 0;
    while (done < n) {
        if (deadline) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                *deadline - std::chrono::steady_clock::now());
            pollfd pfd{sock_.get(), POLLIN, 0};
            const int ready = left.count() <= 0
                                  ? 0
                                  : ::poll(&pfd, 1, static_cast<int>(std::min<long long>(
                                                        left.count(), 1'000'000'000LL)));
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw_errno("poll");
            }
            if (ready == 0) {
                throw Timeout("peer did not answer in time");
            }
        }
        iovec iov{dst + done, n - done};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        // Room for a few descriptors, so a peer sending several cannot make
        // the kernel silently truncate them (MSG_CTRUNC) without us noticing.
        alignas(cmsghdr) std::array<char, kCmsgSpace * 4> control{};
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();

        const ssize_t got = ::recvmsg(sock_.get(), &msg, MSG_CMSG_CLOEXEC);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("recvmsg");
        }

        bool extra = false;
        UniqueFd fd = collect_fds(msg, extra);
        if (fd && got_fd) {
            extra = true;  // a second fd within one frame
        } else if (fd) {
            got_fd = std::move(fd);
        }
        if (extra || (msg.msg_flags & MSG_CTRUNC) != 0) {
            throw ProtocolError("peer sent more than one file descriptor");
        }
        if (got_fd && !accept_fds_) {
            throw ProtocolError("peer sent a file descriptor");
        }

        if (got == 0) {
            if (done == 0 && eof_ok) {
                return false;
            }
            throw ProtocolError("connection closed mid-frame");
        }
        done += static_cast<std::size_t>(got);
    }
    return true;
}

std::optional<Frame> Channel::recv() { return recv_until(std::nullopt); }

std::optional<Frame> Channel::recv(std::chrono::milliseconds timeout) {
    return recv_until(std::chrono::steady_clock::now() + timeout);
}

std::optional<Frame> Channel::recv_until(Deadline deadline) {
    std::array<std::uint8_t, kHeaderBytes> hdr{};
    UniqueFd fd;
    if (!read_exact(hdr.data(), hdr.size(), fd, /*eof_ok=*/true, deadline)) {
        return std::nullopt;
    }

    Reader r(hdr);
    const std::size_t length = r.u32();
    const std::uint16_t type = r.u16();
    const std::uint64_t id = r.u64();
    if (!is_known(type)) {
        throw ProtocolError("unknown message type");
    }
    if (length > kMaxPayload) {
        throw ProtocolError("payload exceeds the protocol maximum");
    }

    Frame f;
    f.type = static_cast<MsgType>(type);
    f.id = id;
    f.payload.resize(length);
    if (length > 0) {
        read_exact(f.payload.data(), length, fd, /*eof_ok=*/false, deadline);
    }
    // Only Open and Save legitimately carry a descriptor.
    if (fd && !takes_fd(f.type)) {
        throw ProtocolError("file descriptor attached to a message that takes none");
    }
    f.fd = std::move(fd);
    return f;
}

void Channel::shutdown() noexcept { ::shutdown(sock_.get(), SHUT_RDWR); }

}  // namespace leht::ipc
