// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/ipc/channel.hpp"

#include <sys/types.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace leht::ipc {

/// The file descriptor on which leht-worker finds its socket to the viewer.
inline constexpr int kWorkerSocketFd = 3;

/// How a worker process ended.
struct ExitStatus {
    bool signaled = false;  ///< true: killed by `code`; false: exited with `code`
    int code = 0;

    /// Anything but a clean exit(0).
    [[nodiscard]] bool crashed() const noexcept { return signaled || code != 0; }
};

/// A running leht-worker, with the viewer's end of its socket.
///
/// Destroying it shuts the socket (the worker sees EOF and exits), waits
/// briefly, and then SIGKILLs and reaps it, so no zombie outlives this.
class WorkerProcess {
public:
    /// Starts `exe` with `args`, its socket on fd kWorkerSocketFd. No other
    /// descriptor of ours is inherited. Throws std::system_error on failure.
    static std::unique_ptr<WorkerProcess> spawn(const std::string& exe,
                                                const std::vector<std::string>& args = {});

    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;
    ~WorkerProcess();

    /// Sends Hello and checks the HelloAck's protocol version. Throws
    /// ProtocolError on a mismatch or anything unexpected.
    void handshake();

    [[nodiscard]] Channel& channel() noexcept { return *channel_; }
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    /// Non-blocking: the exit status if the worker has ended.
    std::optional<ExitStatus> try_wait();

    /// Waits up to `timeout` for the worker to end; nullopt if it has not.
    std::optional<ExitStatus> wait_for(std::chrono::milliseconds timeout);

    /// SIGKILLs the worker and reaps it.
    ExitStatus kill();

private:
    WorkerProcess(pid_t pid, UniqueFd sock);

    pid_t pid_ = -1;
    std::optional<ExitStatus> status_;
    std::unique_ptr<Channel> channel_;
};

}  // namespace leht::ipc
