// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sandbox.hpp"

#include "leht/ipc/channel.hpp"

#include <string>

namespace leht::worker {

/// Serves Recognize requests on `channel` until EOF or Shutdown: loads
/// `languages` first, then (if `sandbox`) applies the sandbox, then reads
/// pixels only. `load_late` loads the languages after the sandbox instead,
/// which kills the process -- the test that proves the order matters.
int run_ocr_worker(ipc::Channel& channel, const std::string& languages, bool sandbox,
                   const SandboxOptions& options, bool load_late);

}  // namespace leht::worker
