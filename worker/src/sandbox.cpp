// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sandbox.hpp"

#include <seccomp.h>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#if defined(__SANITIZE_ADDRESS__)
#define LEHT_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LEHT_SANITIZED 1
#endif
#endif

namespace leht::worker {

namespace {

[[noreturn]] void fail(const std::string& what, int err) {
    throw std::runtime_error("sandbox: " + what + ": " + std::strerror(err));
}

void set_limit(int resource, rlim_t value, const char* name) {
    const rlimit lim{value, value};
    if (::setrlimit(resource, &lim) != 0) {
        fail(std::string("setrlimit ") + name, errno);
    }
}

/// SIGSYS handler for debug mode: report which syscall was refused, then die.
/// Async-signal-safe: formats by hand and uses only write() and _exit(), both
/// of which the policy allows.
void report_sigsys(int /*sig*/, siginfo_t* info, void* /*ctx*/) {
    char msg[] = "leht-worker: seccomp refused syscall ______\n";
    int nr = info->si_syscall;
    char* p = msg + std::strlen("leht-worker: seccomp refused syscall ");
    char digits[8];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + nr % 10);
        nr /= 10;
    } while (nr > 0 && n < 6);
    for (int i = 0; i < 6; ++i) {
        p[i] = i < n ? digits[n - 1 - i] : ' ';
    }
    (void)!::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    ::_exit(128 + SIGSYS);
}

/// RAII for the libseccomp filter context.
struct Filter {
    scmp_filter_ctx ctx;
    explicit Filter(std::uint32_t def) : ctx(seccomp_init(def)) {
        if (ctx == nullptr) {
            throw std::runtime_error("sandbox: seccomp_init failed");
        }
    }
    ~Filter() { seccomp_release(ctx); }
    Filter(const Filter&) = delete;
    Filter& operator=(const Filter&) = delete;

    void allow(const char* name) { rule(SCMP_ACT_ALLOW, name, 0, nullptr); }

    void rule(std::uint32_t action, const char* name, unsigned n, const scmp_arg_cmp* args) {
        const int nr = seccomp_syscall_resolve_name(name);
        if (nr == __NR_SCMP_ERROR) {
            return;  // not a syscall on this architecture; nothing to allow
        }
        const int rc = seccomp_rule_add_array(ctx, action, nr, n, args);
        if (rc != 0) {
            fail(std::string("seccomp rule for ") + name, -rc);
        }
    }
};

// SCMP_A*() are C macros expanding to compound literals of scmp_arg_cmp; build
// the structs directly instead so the arguments are plain C++.
scmp_arg_cmp arg_masked_eq(unsigned arg, scmp_datum_t mask, scmp_datum_t value) {
    return scmp_arg_cmp{arg, SCMP_CMP_MASKED_EQ, mask, value};
}
scmp_arg_cmp arg_eq(unsigned arg, scmp_datum_t value) {
    return scmp_arg_cmp{arg, SCMP_CMP_EQ, value, 0};
}

void install_seccomp(bool debug) {
    Filter f(debug ? SCMP_ACT_TRAP : SCMP_ACT_KILL_PROCESS);

    // I/O on descriptors we already hold: the socket, and the document fd the
    // viewer passes with each Open. Nothing can open a new one.
    for (const char* name : {"read", "readv", "pread64", "write", "writev", "lseek",
                             "close", "fstat", "recvmsg", "sendmsg", "shutdown"}) {
        f.allow(name);
    }
    // fstat() in current glibc is fstatat(fd, "", AT_EMPTY_PATH): allow only
    // that form, so no path can be probed.
    {
        const scmp_arg_cmp a[] = {arg_masked_eq(3, AT_EMPTY_PATH, AT_EMPTY_PATH)};
        f.rule(SCMP_ACT_ALLOW, "newfstatat", 1, a);
        f.rule(SCMP_ACT_ALLOW, "fstatat64", 1, a);
    }
    {
        const scmp_arg_cmp a[] = {arg_masked_eq(2, AT_EMPTY_PATH, AT_EMPTY_PATH)};
        f.rule(SCMP_ACT_ALLOW, "statx", 1, a);
    }
    // fcntl only to read or set descriptor flags.
    for (const int cmd : {F_GETFD, F_SETFD, F_GETFL}) {
        const scmp_arg_cmp a[] = {arg_eq(1, static_cast<scmp_datum_t>(cmd))};
        f.rule(SCMP_ACT_ALLOW, "fcntl", 1, a);
    }

    // Memory. Never executable: a parser exploit gets no fresh place to put
    // code. (mmap of an fd is harmless: the only fds are ones we were given.)
    for (const char* name : {"mmap", "mprotect", "pkey_mprotect"}) {
        const scmp_arg_cmp a[] = {arg_masked_eq(2, PROT_EXEC, 0)};
        f.rule(SCMP_ACT_ALLOW, name, 1, a);
    }
    for (const char* name : {"munmap", "mremap", "brk", "madvise"}) {
        f.allow(name);
    }

    // Threads: the reader thread, and nothing that makes a new process.
    // clone3 passes its flags in memory where BPF cannot see them, so refuse
    // it with ENOSYS -- glibc then falls back to clone(), whose flags we can
    // check.
    f.rule(SCMP_ACT_ERRNO(ENOSYS), "clone3", 0, nullptr);
    {
        const scmp_arg_cmp a[] = {arg_masked_eq(0, CLONE_THREAD, CLONE_THREAD)};
        f.rule(SCMP_ACT_ALLOW, "clone", 1, a);
    }
    for (const char* name : {"futex", "futex_waitv", "set_robust_list", "rseq",
                             "sched_yield", "sched_getaffinity", "membarrier"}) {
        f.allow(name);
    }

    // Signals and exits. abort() needs getpid/gettid/tgkill to raise SIGABRT
    // on ourselves -- a crash must still look like a crash.
    for (const char* name : {"rt_sigreturn", "rt_sigprocmask", "rt_sigaction", "sigaltstack",
                             "getpid", "gettid", "tgkill", "exit", "exit_group",
                             "restart_syscall"}) {
        f.allow(name);
    }

    // Time and entropy (usually vDSO, but the fallbacks are syscalls).
    for (const char* name : {"clock_gettime", "clock_getres", "gettimeofday",
                             "clock_nanosleep", "nanosleep", "getrandom"}) {
        f.allow(name);
    }

    const int rc = seccomp_load(f.ctx);
    if (rc != 0) {
        fail("seccomp_load", -rc);
    }
}

}  // namespace

bool sandbox_supported(std::string* why) {
#ifdef LEHT_SANITIZED
    if (why != nullptr) {
        *why = "sanitizer build: ASan/LSan need syscalls the sandbox denies";
    }
    return false;
#else
    (void)why;
    return true;
#endif
}

void apply_sandbox(const SandboxOptions& options) {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fail("PR_SET_NO_NEW_PRIVS", errno);
    }

    set_limit(RLIMIT_CORE, 0, "RLIMIT_CORE");
    // stdio + socket + one document fd, with headroom for a document being
    // replaced by the next Open.
    set_limit(RLIMIT_NOFILE, 16, "RLIMIT_NOFILE");
    if (options.address_space_bytes != 0) {
        set_limit(RLIMIT_AS, static_cast<rlim_t>(options.address_space_bytes), "RLIMIT_AS");
    }

    // Best effort: unprivileged user namespaces may be disabled by policy.
    // seccomp below is the real barrier; this is depth behind it.
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWIPC) != 0) {
        (void)::unshare(CLONE_NEWNET | CLONE_NEWIPC);  // works where we are privileged
    }

    if (options.debug) {
        struct sigaction sa {};
        sa.sa_sigaction = report_sigsys;
        sa.sa_flags = SA_SIGINFO;
        ::sigaction(SIGSYS, &sa, nullptr);
    }
    install_seccomp(options.debug);
}

}  // namespace leht::worker
