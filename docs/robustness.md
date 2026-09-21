# Robustness and untrusted input

A PDF is an untrusted input. People open files that arrived by email, from a scanner they
don't control, or off a website. This file records what we know about how the engine
behaves on hostile input, and what Leht does about it. Since M3 the viewer parses nothing
itself: see [Process isolation](#process-isolation).

## What has been found

| Finding | Whose | Status |
|---|---|---|
| MuPDF aborts on the five bytes `obj<<` | upstream | fixed in 1.28.4 |
| Uncontrolled format string in `ops::split` | **ours** | fixed |
| `memcpy` from a null pointer in `renderer.cpp` | **ours** | fixed |
| Three leaked MuPDF object references in the ops layer | **ours** | fixed |
| `pdf_save_document` leaks ~874 bytes per call | upstream | **live** |
| Stack overflow on deep indirect-reference chains | upstream | **live** |
| Stack overflow loading a deeply nested outline | upstream | **live** — contained by M3 |
| Quadratic page lookup on a broken page tree | upstream | **live** — contained by the request timeout |

Three of the eight were ours. A robustness document that only catalogues other people's
defects is marketing, so they are written up here at the same length as the upstream ones.

## Upstream: MuPDF aborts on five bytes — resolved

The fuzz target found a memory-safety bug on its first real run: five ASCII bytes,
`obj<<`, abort **MuPDF 1.28.2** with `free(): invalid pointer` inside
`pdf_repair_xref_aux`. The artifact, a 15-line pure-C reproducer and the full stack are in
[`tests/crashes/README.md`](../tests/crashes/README.md).

**It was upstream, not ours** — the reproducer contains no Leht code and calls nothing but
`fz_open_document`, and it was independently rebuilt and rerun to confirm that.

**It is fixed in MuPDF 1.28.4.** Nothing to report to Artifex.

| Version | Behaviour on `obj<<` |
|---|---|
| 1.28.2 (Fedora 44, 2026-08-04) | `free(): invalid pointer`, SIGABRT |
| 1.28.4 (upstream, 2026-09-15) | `syntax error: invalid key in dict`, clean reject, exit 0 |

Not one lucky input: the same mutation driver, compiled twice against the two libraries
with an identical corpus and identical seeding, crashed 1.28.2 inside 4,000 iterations and
survived **64,000 iterations across four seeds** on 1.28.4.

**The attribution is not established.** 1.28.4's changelog mentions a double-free in PDF
structure tree parsing — a *different* code path from `pdf_repair_xref_aux`. The behaviour
change is unambiguous; which commit caused it is not. Do not write that the changelog line
is the fix.

**What must never be claimed.** "Not matched to a public report" is not "novel". This was
found in the version Fedora ships, and upstream had already fixed it. Nobody describes it
as a discovered CVE.

## The durable lesson

The bug is gone; the lesson isn't, and it has nothing to do with MuPDF. **The version a
distribution ships is not the version upstream supports.** Fedora 44 shipped 1.28.2 while
upstream was on 1.28.4 — a six-week gap containing a crash on five bytes. For any library
that parses hostile input, that gap is a security gap.

This resolved well and is mildly *in MuPDF's favour* — the fix existed before we found the
bug. Next time it may not. `cmake/FindMuPDF.cmake` therefore warns when MuPDF is older than
1.28.4, as a warning rather than a hard requirement: demanding 1.28.4 would make Leht
unbuildable on Fedora 44, which is its own development platform, and that costs more than
it buys.

## Keeping it proportionate

This is not a reason to abandon MuPDF. Every C PDF parser has carried bugs of this class;
poppler and pdfium both have long CVE histories. That the repair path gave up a crash after
22 mutation iterations suggests that particular path is not heavily fuzzed upstream, which
is useful information, not a verdict on the library.

What it does establish is that **the parser will crash on some inputs, and no amount of
care in `core/` prevents that**, because the fault is below us.

## Ours: an uncontrolled format string in `split` — fixed

`ops::split` passed the caller's output pattern to `snprintf` **as its format string**.
That makes a filename template into attacker-controlled printf input — CWE-134:

```
leht split in.pdf -o '%s.pdf'    # SIGSEGV: page number dereferenced as a pointer
leht split in.pdf -o '%n.pdf'    # would be an arbitrary write
```

It was found by trying it, not by a tool, the day it was written.

The aggravating detail is that the call sat underneath a
`#pragma GCC diagnostic ignored "-Wformat-nonliteral"` added by the same change that
introduced the bug. That warning exists precisely to name this defect class. The compiler
reported it and was silenced.

`split` now expands the pattern itself and never hands user input to printf at all: `%d`,
zero-padded `%0Nd` and `%%` are accepted, exactly one integer field is required, and
everything else — `%s`, `%n`, `%p`, extra fields, absurd widths — is rejected *before* the
input is opened, so a bad pattern cannot half-finish a split. Each dangerous pattern has
its own regression test.

This is why [CONTRIBUTING.md](../CONTRIBUTING.md) now requires a justifying comment on any
diagnostic pragma. The rule exists because of this bug.

## Ours: four defects the first time sanitizers ran — fixed

Sanitizers were configured early and then not actually run for some time, because the
runtime packages were missing. The first real pass over the suite found four defects:

- `renderer.cpp` — `memcpy` with a null source and destination when a degenerate page
  produced a pixmap with no samples. Undefined behaviour even at zero length.
- `ops/pages.cpp` — `rotate()` called `pdf_dict_put(..., pdf_new_int(...))`. The dictionary
  takes its own reference, so the one `pdf_new_int` returned leaked.
- `ops/compress.cpp` — the same mistake three more times.
- `ops/merge.cpp` — an XObject dictionary created and never dropped.

**All four were in code whose functional tests were passing.** Sixty-two green tests said
nothing was wrong. Correctness tests and memory-safety tests answer different questions,
and passing the first says nothing about the second.

## Upstream: a leak in every save — live

`pdf_save_document` leaks roughly **874 bytes per call** when `do_garbage >= 2`, in the
object renumbering path. Confirmed with a pure-C reproducer against **both 1.28.2 and
1.28.4**, so it is long-standing rather than a regression, and it accumulates linearly:
50 saves leak 43,700 bytes. Details and the reproducer are in
[`tests/crashes/README.md`](../tests/crashes/README.md).

Unlike the `obj<<` crash, **this one is still live in current upstream and is worth
reporting to Artifex.** That has not been done yet.

Leht keeps `do_garbage = 3` regardless. De-duplication is worth far more than 874 bytes —
merging four copies of one file without it costs four copies of their shared fonts, which
`test_merge` asserts. For the CLI the leak is irrelevant because the process exits. It will
matter for the M2 viewer if a session performs many saves, and that is the point to
revisit it, not now.

## Upstream: stack overflow on deep reference chains — live

A PDF whose objects form a long indirect-reference chain (4 -> 5 -> 6 -> ...)
overflows the stack: `pdf_resolve_indirect` -> `pdf_cache_object` recurses once
per link, and ~200,000 links (a ~10 MB file) exhaust a default 8 MB stack. Pure
MuPDF crashes identically with no Leht code, and ASan on **1.28.4** reports a
stack-overflow in `pdf_cache_object`, so it is live in current upstream.
Details and a generator: [`../tests/crashes/README.md`](../tests/crashes/README.md).

Lower severity than the others — a crash / DoS, not corruption, and it needs a
multi-megabyte crafted file. But it is the sharpest case for process isolation,
because there is **no in-process fix**: the recursion is MuPDF's and MuPDF
exposes no depth limit for Leht to cap. This is exactly the situation
process isolation exists for.

It only fires when something walks the whole object graph — save, `compress` — so it is
reached from the CLI and the write-side ops, which by design still run in-process (see
below). A crash there costs one `leht` command, not an open session.

## Upstream: stack overflow loading a deep outline — live, contained

Found 2026-09-21 while looking for a real viewer-path crash to test M3 against. An outline
nested ~75,000 levels deep (each item's `/First` is the next; a ~8 MB file) overflows the
stack inside a single `fz_load_outline` call: `pdf_test_outline` validates the tree by
recursing once per level with no cap. Pure MuPDF crashes identically — reproducer and
generator in [`../tests/crashes/README.md`](../tests/crashes/README.md) — in both 1.28.2 and
1.28.4.

Same class and severity as the reference chain, but it is **on every viewer's path**:
loading the outline is what a viewer does straight after opening a file. Before M3,
double-clicking this file killed the Leht viewer. Now it kills one `leht-worker`, the
viewer reports that the file could not be opened safely, and the next file opens normally.
It is the fixture the worker and viewer tests use to prove exactly that.

## Upstream: quadratic page lookup on a broken page tree — live, contained

Found by `fuzz_ops` as a timeout. One unparsable entry in a page tree's `/Kids` stops
MuPDF building its page map. Every page lookup then walks the tree, and re-parses the
broken object each time because failed parses are not cached. 16,000 pages take ~8–10 s
to size, against 0.1 s without the bad entry, and the cost grows with the square of the
page count. Generator and pure-C reproducer in
[`../tests/crashes/README.md`](../tests/crashes/README.md); live in 1.28.2 and 1.28.4.

It also exposed a gap in M3 as first built: nothing bounded how long the worker could
take, so a stalled parser left a document that never loaded. The viewer now gives every
reply a deadline and treats a miss like a crash — see below.

## Fuzzing coverage to date

Against MuPDF 1.28.4, fuzzing has found no crash or memory error in Leht's own code.
`fuzz_ipc` (M3) targets the viewer's decoder for worker output — raw bytes through
`Channel::recv()` and every message decoder — since that is what a compromised worker
controls:

| Target | Driver | Executions | Result |
|---|---|---|---|
| `fuzz_open` | libFuzzer, MuPDF instrumented | ~60,000 (6 jobs x 15 min) | clean |
| `fuzz_open` | libFuzzer, wrapper-only coverage | 587,754 | clean |
| `fuzz_open` | mutation driver | 64,000 | clean |
| `fuzz_ops` | libFuzzer | 56,285 | clean |
| `fuzz_ops` | mutation driver | 15,000+ | clean |
| `fuzz_ops` | libFuzzer, MuPDF instrumented, ASan/UBSan | 14,302 (4 jobs x 30 min, 4,032 edges) | one timeout → quadratic page lookup (upstream); an encrypt-path leak under investigation |
| `fuzz_ipc` | libFuzzer + ASan/UBSan | 6,751,008 (4 jobs x 15 min) | clean |
| `fuzz_ipc` | mutation driver + ASan/UBSan | 200,000 | clean |

"Nothing found yet", not "nothing there". Two caveats matter:

- Only the last `fuzz_open` runs could see *inside* MuPDF. Earlier runs built MuPDF
  without coverage instrumentation, so libFuzzer got feedback only from Leht's thin
  wrapper (382 counters) and mutated blindly through the parser. Rebuilding MuPDF with
  `-fsanitize=fuzzer-no-link` raised that to 202,151 counters and coverage climbed from a
  flat ~200 to ~7,500 — that is the run that actually exercised the parser.
- The known crashes above were *not* found by fuzzing. `obj<<` came from the mutation
  driver, and the stack overflow and the bombs came from hand-written adversarial inputs.
  Structural pathologies — deep chains, huge declared dimensions — are hard for a byte
  mutator to stumble onto, which is why targeted probing still earns its place.

## Process isolation

**Built in M3.** The viewer never parses a document. Each open spawns a fresh
`leht-worker` process, passes it the already-open file descriptor, and receives only plain
values back. A crash inside MuPDF costs that process; the viewer survives it.

```
leht-viewer (trusted)                    leht-worker (sandboxed, one per document)
  opens the file, passes the fd  ─────►   Document::open_fd, then MuPDF
  page cache, UI, clipboard      ◄─────   page sizes, outline rows, RGB bitmaps,
  validates every frame                   text quads, selection text -- nothing else
```

### Threat model

A hostile PDF may crash MuPDF (as the two live overflows above do) or, worse, achieve code
execution inside it. Isolation handles both:

- **A crash** ends the worker. The viewer sees end-of-stream, never a half-written frame
  it acts on.
- **A stall** — a parser loop, or input built to be slow — is bounded too: every reply
  must arrive within 30 s (`LEHT_WORKER_TIMEOUT_MS` overrides it), the whole frame
  inside that deadline so a worker trickling bytes cannot stretch it. A miss kills the
  worker and counts against the file exactly as a crash does.
- **Code execution** is trapped in a process that can do almost nothing (below). Its only
  channel out is the socket to the viewer, so the viewer treats that socket as hostile: the
  decoder in `ipc/` bounds every length, rejects element counts the payload cannot hold,
  requires finite floats and sane pages, rotations and zooms, and checks that a bitmap's
  stride × height accounts for exactly the bytes it carries. A malformed frame is taken as
  proof the worker is compromised: it is killed and never read from again. The decoder has
  its own fuzz target, `fuzz_ipc`.
- **Cross-document leakage** is prevented by giving every document a fresh worker: nothing
  one file does to a worker can reach the next file opened.

### The sandbox

Applied by the worker after MuPDF initialises and before the first untrusted byte arrives
(`worker/src/sandbox.cpp`):

1. `PR_SET_NO_NEW_PRIVS`.
2. rlimits: no core dumps; 16 descriptors; 4 GB of address space, which also turns an
   allocation bomb into `bad_alloc` and an ordinary "out of memory" reply.
3. New user, network and IPC namespaces — best effort, since unprivileged user namespaces
   can be disabled by policy. The tests report whether the network namespace took.
4. A seccomp-bpf allowlist, **killing the process** on anything else: I/O on descriptors
   already held; memory, never executable; threads via `clone` with `CLONE_THREAD` only
   (`clone3` gets `ENOSYS` so glibc falls back to the filterable call); futexes; the signal
   calls `abort()` needs; time and entropy. No `open`, `socket`, `exec`, `fork`, or
   executable mapping. `fstatat`/`statx` are allowed only in their `AT_EMPTY_PATH` form, so
   no path can even be probed.

MuPDF here compiles its fonts in and links no fontconfig, so text in non-embedded fonts —
CJK included — renders with no filesystem access; that was checked under the sandbox.

Each forbidden action has a CTest (`worker_sandbox_denies_*`) requiring death by `SIGSYS`
specifically. To extend the list after a MuPDF upgrade, run with
`LEHT_WORKER_SECCOMP_DEBUG=1`: refusals then print the syscall number instead of killing
silently. `--no-sandbox` / `LEHT_WORKER_NO_SANDBOX=1` exist for debugging. **Sanitizer
builds run the worker unsandboxed** — LSan's ptrace stop-the-world and ASan's shadow
reservation are incompatible with the policy — so the sandbox is exercised by the ordinary
Debug and Release test runs.

### When a worker dies

First the viewer decides whether the death is evidence against the file. A crash signal
(`SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGSYS` from the sandbox, ...), a non-zero exit (a
sanitizer report), a malformed frame or a missed deadline it was killed for — **yes**. `SIGKILL`/`SIGTERM`
from outside — the kernel OOM killer, a user's `kill` — **no**.

| When | Blamed on the file | Killed from outside |
|---|---|---|
| During open (or reading the outline) | `failed()`: "could not open this file safely"; quarantined for the session | fresh worker, open retried |
| During a page render or selection | that page is drawn as a labelled "could not be displayed safely" placeholder and never retried; a fresh worker reopens the document (re-unlocking it with the password already given) and every other page keeps working | fresh worker, same request retried once |
| During search | matches so far stand, search finishes; document restored | same |
| A second time in one document | document closed and quarantined | — |
| A fourth outside kill in one document | — | document closed with "repeatedly terminated from outside"; **not** quarantined |

Quarantine is keyed on (device, inode, size, mtime), so reopening the same file fails
fast without spawning anything, while an edited copy gets a fresh chance.

### What it costs

`bench/worker_latency.cpp`, Release, i7-13700H, median of 7:

| | in-process | worker | overhead |
|---|---|---|---|
| open + first page, `text_10p` | 10.8 ms | 24.3 ms | +13.5 ms |
| open + first page, `text_500p` | 18.2 ms | 37.5 ms | +19.3 ms |
| uncached page turn (3.1 MB bitmap) | ~5 ms | ~8.6 ms | ~+3.5 ms |
| cached page turn | — | — | none: the cache lives in the viewer |

The open overhead is almost all process start (~15–17 ms for spawn + handshake); the
per-page overhead is copying the bitmap across. Two tempting fixes were measured and
dropped as noise: a pre-spawned spare worker (with a built-in warm-up page), and larger
socket buffers. Shared-memory bitmap transport is the next lever if a target is ever missed.

The 500-page open used to cost ~190–350 ms (load-dependent) before a single pixel,
because `Renderer::page_size` built a full display list — interpreting every page's
content — just to read its dimensions, and the viewer sizes every page on open. It now
reads the page dictionary instead (`fz_bound_page`, identical bounds), and each page's
content is parsed on its first render. Measured back to back under the same load:
348 → 18 ms in-process, 367 → 38 ms through the worker.

### What is deliberately not isolated

- **The CLI and write-side ops** (`merge`, `compress`, `encrypt`, ...) run in-process with
  one independent `Context` each, as before. A crash costs one command, and the batch
  model already confines it.
- **Windows and macOS** equivalents (job objects and restricted tokens; App Sandbox) are
  scoped with those platforms.

Both properties that made this incremental still matter, so keep them: no MuPDF type in a
public `core/` header (the API surface *is* the wire format), and one independent
`leht::Context` per thread or process (see [threading.md](threading.md)).

## Standing rules

- Crash artifacts live in `tests/crashes/`, **never** in `tests/corpus/`. A corpus file
  that aborts turns the test suite red for a reason unrelated to the change under test.
- A defect found in a dependency gets a pure-library reproducer before it is called
  upstream. "It crashes in our tool" is not a bug report. This applies to leaks as much as
  crashes — both upstream findings here have one.
- Suppress a leak only when it is upstream and has a reproducer. **Never suppress one of
  ours.** `tests/lsan.supp` is the whole list and every entry carries its reasoning.
- The mutation fuzzer is run by hand, not in CI, and the reason is written down in
  [CONTRIBUTING.md](../CONTRIBUTING.md). A fuzz target nobody runs is not protection.

## Outstanding

- [x] ~~Verify `obj<<` upstream~~ — fixed in 1.28.4; no report needed
- [x] ~~Install `libasan`/`libubsan`~~ — done; the first run found four defects of ours
- [x] ~~Install `clang` for coverage-guided libFuzzer~~ — done; 644,039 executions, clean
- [ ] **Report four live upstream bugs to Artifex** (bugs.ghostscript.com, MuPDF
      component): the `pdf_save_document` leak (pure-C reproducer + patch ready), the
      indirect-reference-chain stack overflow (generator ready), the outline-depth
      stack overflow, and the quadratic page lookup (both: pure-C reproducer +
      generator ready). None sent yet.
- [ ] Install `llvm-symbolizer` (Fedora `llvm`) so LSan suppressions resolve under
      libFuzzer. Without it libFuzzer must run with `-detect_leaks=0`.
- [ ] Give `fuzz_ops` far more time. The 2026-09-22 run (30 min, instrumented MuPDF) ran at
      ~1 exec/s on the 500-page seed and still found a timeout; trim the seed corpus to
      small files so it explores faster.
- [x] ~~Decide when process isolation lands~~ — built in M3, straight after the viewer
- [x] ~~Lazy page sizes~~ — `page_size` reads page bounds, not content; a 500-page open
      went from ~350 ms to ~20 ms
