// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sandbox.hpp"

#include "leht/ipc/channel.hpp"

namespace leht::worker {

/// leht-worker --trusted-list: serves TrustedListStep requests on `channel`
/// until EOF or Shutdown. libxml2 and xmlsec1 are initialised first, then (if
/// `sandbox`) the sandbox goes up, then it reads the lists the viewer fetched.
/// `init_late` initialises after the sandbox instead -- only a test asks, to
/// see whether that order matters.
int run_trustlist_worker(ipc::Channel& channel, bool sandbox, const SandboxOptions& options,
                         bool init_late);

}  // namespace leht::worker
