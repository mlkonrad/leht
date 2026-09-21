// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ipc/process.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <thread>

extern char** environ;

namespace leht::ipc {

namespace {

[[noreturn]] void throw_errno(int err, const char* what) {
    throw std::system_error(err, std::generic_category(), what);
}

ExitStatus decode_status(int status) {
    if (WIFSIGNALED(status)) {
        return {true, WTERMSIG(status)};
    }
    return {false, WEXITSTATUS(status)};
}

/// posix_spawn_file_actions_t with RAII cleanup.
struct FileActions {
    posix_spawn_file_actions_t fa{};
    FileActions() { posix_spawn_file_actions_init(&fa); }
    ~FileActions() { posix_spawn_file_actions_destroy(&fa); }
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;
};

}  // namespace

std::unique_ptr<WorkerProcess> WorkerProcess::spawn(const std::string& exe,
                                                    const std::vector<std::string>& args) {
    auto [ours, theirs] = socket_pair();

    // dup2 onto the fixed fd clears close-on-exec -- unless source and target
    // are the same number, where it is a no-op and the fd would be closed at
    // exec. Move our copy well clear of kWorkerSocketFd first.
    UniqueFd child_end(::fcntl(theirs.get(), F_DUPFD_CLOEXEC, 10));
    if (!child_end) {
        throw_errno(errno, "fcntl(F_DUPFD_CLOEXEC)");
    }
    theirs.reset();

    FileActions actions;
    posix_spawn_file_actions_adddup2(&actions.fa, child_end.get(), kWorkerSocketFd);

    std::vector<std::string> argv_store;
    argv_store.push_back(exe);
    argv_store.insert(argv_store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : argv_store) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe.c_str(), &actions.fa, nullptr, argv.data(), environ);
    if (rc != 0) {
        throw_errno(rc, "posix_spawn leht-worker");
    }
    return std::unique_ptr<WorkerProcess>(new WorkerProcess(pid, std::move(ours)));
}

WorkerProcess::WorkerProcess(pid_t pid, UniqueFd sock)
    : pid_(pid), channel_(std::make_unique<Channel>(std::move(sock), /*accept_fds=*/false)) {}

WorkerProcess::~WorkerProcess() {
    channel_->shutdown();
    if (!wait_for(std::chrono::milliseconds(200))) {
        kill();
    }
}

void WorkerProcess::handshake() {
    channel_->send(0, Hello{});
    auto reply = channel_->recv();
    if (!reply) {
        throw ProtocolError("worker closed the connection during handshake");
    }
    if (decode_as<HelloAck>(*reply).version != kProtocolVersion) {
        throw ProtocolError("worker speaks a different protocol version");
    }
}

std::optional<ExitStatus> WorkerProcess::try_wait() {
    if (status_) {
        return status_;
    }
    int status = 0;
    pid_t r = 0;
    do {
        r = ::waitpid(pid_, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == pid_) {
        status_ = decode_status(status);
    }
    return status_;
}

std::optional<ExitStatus> WorkerProcess::wait_for(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!try_wait()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return status_;
}

ExitStatus WorkerProcess::kill() {
    if (!try_wait()) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        pid_t r = 0;
        do {
            r = ::waitpid(pid_, &status, 0);
        } while (r < 0 && errno == EINTR);
        status_ = r == pid_ ? decode_status(status) : ExitStatus{true, SIGKILL};
    }
    return *status_;
}

}  // namespace leht::ipc
