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

#include "session.hpp"

#include "leht/context.hpp"
#include "leht/ipc/process.hpp"

#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <exception>
#include <string_view>

int main(int argc, char** argv) {
    // Die with the viewer, whatever happens to it.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);

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

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::fprintf(stderr, "leht-worker: unknown option %.*s\n",
                     static_cast<int>(arg.size()), arg.data());
        return 2;
    }

    try {
        leht::ipc::Channel channel(leht::ipc::UniqueFd(leht::ipc::kWorkerSocketFd),
                                   /*accept_fds=*/true);
        leht::Context ctx;
        leht::worker::Session session(channel, ctx);
        return session.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "leht-worker: %s\n", e.what());
        return 1;
    }
}
