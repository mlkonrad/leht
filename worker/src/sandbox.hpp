// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace leht::worker {

/// Locks the worker down before it touches any untrusted byte. See
/// docs/robustness.md, "Process isolation", for the threat model.
///
/// In order:
///   1. PR_SET_NO_NEW_PRIVS, so nothing later can regain privilege.
///   2. rlimits: no core dumps, few descriptors, bounded address space (the
///      last also turns an allocation bomb into bad_alloc, i.e. a Failed reply).
///   3. Best effort: new user + network + IPC namespaces, so even a seccomp
///      escape finds no network.
///   4. A seccomp-bpf allowlist. Anything else KILLS the process, which the
///      viewer handles like any other crash. No open, no socket, no exec, no
///      fork, no executable memory.
///
/// Must be called while the process is single-threaded: unshare() of a user
/// namespace requires it.
struct SandboxOptions {
    /// RLIMIT_AS. 0 leaves it unset.
    std::size_t address_space_bytes = std::size_t{4} << 30;
    /// Replace KILL with TRAP and report the blocked syscall number on stderr
    /// before exiting. For developing the allowlist, never for shipping.
    bool debug = false;
};

/// Applies the sandbox. Throws std::runtime_error if a mandatory layer
/// (no_new_privs, rlimits, seccomp) could not be installed -- the worker must
/// not go on to parse anything unsandboxed by accident.
void apply_sandbox(const SandboxOptions& options);

/// False in sanitizer builds. ASan/LSan need syscalls the policy denies (LSan
/// stops the world with ptrace at exit) and reserve terabytes of address space
/// for shadow memory, so those builds run the worker unsandboxed.
bool sandbox_supported(std::string* why = nullptr);

}  // namespace leht::worker
