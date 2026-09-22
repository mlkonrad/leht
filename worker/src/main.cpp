// SPDX-License-Identifier: AGPL-3.0-or-later
//
// leht-worker: parses documents on the viewer's behalf, in its own process.
//
// The viewer opens a file and hands this process the descriptor; this process
// parses it with MuPDF and sends back only plain values -- page sizes, pixels,
// text quads. If a hostile file crashes MuPDF, it crashes this process and the
// viewer respawns it. See docs/robustness.md, "Process isolation".
//
// Not meant to be run by hand: it expects its socket on fd 3.

#include "sandbox.hpp"

#include "leht/crypto/crypto.hpp"
#include "session.hpp"

#include "leht/context.hpp"
#include "leht/ipc/process.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' && std::string_view(v) != "0";
}

/// --selftest-sandbox=WHAT: install the sandbox, then try one thing it must
/// forbid. Reaching the end means the sandbox failed. The test suite expects
/// death by SIGSYS for each WHAT.
int selftest(std::string_view what) {
    std::string why;
    if (!leht::worker::sandbox_supported(&why)) {
        std::fprintf(stderr, "leht-worker: selftest skipped: %s\n", why.c_str());
        return 77;
    }
    leht::worker::apply_sandbox({});

    if (what == "open") {
        (void)::open("/etc/passwd", O_RDONLY | O_CLOEXEC);
    } else if (what == "socket") {
        (void)::socket(AF_INET, SOCK_STREAM, 0);
    } else if (what == "exec") {
        char* const argv[] = {const_cast<char*>("/bin/true"), nullptr};
        (void)::execv("/bin/true", argv);
    } else if (what == "fork") {
        (void)::fork();
    } else if (what == "mmap-exec") {
        (void)::mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    } else {
        std::fprintf(stderr, "leht-worker: unknown selftest %.*s\n",
                     static_cast<int>(what.size()), what.data());
        return 2;
    }
    std::fprintf(stderr, "leht-worker: SANDBOX FAILED: %.*s was allowed\n",
                 static_cast<int>(what.size()), what.data());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    // Die with the viewer, whatever happens to it.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);

    bool sandbox = !env_set("LEHT_WORKER_NO_SANDBOX");
    leht::worker::SandboxOptions options;
    options.debug = env_set("LEHT_WORKER_SECCOMP_DEBUG");

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--no-sandbox") {
            sandbox = false;
        } else if (arg.starts_with("--selftest-sandbox=")) {
            return selftest(arg.substr(std::string_view("--selftest-sandbox=").size()));
        } else {
            std::fprintf(stderr, "leht-worker: unknown option %.*s\n",
                         static_cast<int>(arg.size()), arg.data());
            return 2;
        }
    }
    if (sandbox && !leht::worker::sandbox_supported()) {
        sandbox = false;
    }

    struct stat st {};
    if (::fstat(leht::ipc::kWorkerSocketFd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        std::fprintf(stderr,
                     "leht-worker: expects a socket on fd %d; it is started by the "
                     "Leht viewer, not by hand\n",
                     leht::ipc::kWorkerSocketFd);
        return 2;
    }
    // Keep stdio and the socket; close anything else we might have inherited.
    ::close_range(leht::ipc::kWorkerSocketFd + 1, ~0U, 0);

    try {
        leht::ipc::Channel channel(leht::ipc::UniqueFd(leht::ipc::kWorkerSocketFd),
                                   /*accept_fds=*/true);
        // MuPDF is initialised first, while it may still read what it needs,
        // and the sandbox goes up before the first untrusted byte arrives.
        leht::Context ctx;
        // OpenSSL the same way: no config file is read (the sandbox forbids
        // opening one), and every algorithm verification can reach for is
        // fetched now, because a lazy fetch later would be a file open, and
        // seccomp answers that with SIGKILL.
        leht::crypto::init(/*load_config=*/false);
        // LEHT_WORKER_NO_PRELOAD skips it, which is not an option anyone
        // should use: the test suite sets it to prove that without the
        // preload the worker dies on the first verification.
        if (!env_set("LEHT_WORKER_NO_PRELOAD")) {
            leht::crypto::preload_algorithms();
        }
        if (sandbox) {
            leht::worker::apply_sandbox(options);
        }
        leht::worker::Session session(channel, ctx);
        return session.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "leht-worker: %s\n", e.what());
        return 1;
    }
}
