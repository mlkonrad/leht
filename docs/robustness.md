# Robustness and untrusted input

A PDF is an untrusted input. People open files that arrived by email, from a scanner they
don't control, or off a website. This file records what we know about how the engine
behaves on hostile input, and what the plan is.

## What has been found

| Finding | Whose | Status |
|---|---|---|
| MuPDF aborts on the five bytes `obj<<` | upstream | fixed in 1.28.4 |
| Uncontrolled format string in `ops::split` | **ours** | fixed |
| `memcpy` from a null pointer in `renderer.cpp` | **ours** | fixed |
| Three leaked MuPDF object references in the ops layer | **ours** | fixed |
| `pdf_save_document` leaks ~874 bytes per call | upstream | **live** |

Three of the five were ours. A robustness document that only catalogues other people's
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

## Fuzzing coverage to date

Against MuPDF 1.28.4, no defect has been found in Leht's own code by fuzzing:

| Target | Driver | Executions | Result |
|---|---|---|---|
| `fuzz_open` | libFuzzer, coverage-guided | 587,754 | clean |
| `fuzz_open` | mutation driver | 64,000 | clean |
| `fuzz_ops` | libFuzzer, coverage-guided | 56,285 | clean |
| `fuzz_ops` | mutation driver | 15,000 | clean |

Read that as "nothing found yet", not "nothing there". `fuzz_ops` runs at roughly 133
executions per second against `fuzz_open`'s 3,892, because each input is written to a file
and put through eight operations — so it has had far less exercise than the raw number
suggests, on the more dangerous code.

## Direction: process isolation

Untrusted input eventually wants to be parsed in a separate process, so a parser crash
costs a subprocess rather than the whole application — the model Chrome uses for pdfium.
That is the right long-term answer and it is not yet built.

`core/` is already shaped for it, by accident of two earlier decisions:

- No MuPDF type crosses the public API — only forward-declared opaque pointers — so the
  boundary a subprocess would sit on already exists.
- Batch work already uses one independent `leht::Context` per worker rather than shared
  cloned contexts (see [threading.md](threading.md)), so there is no shared parser state to
  untangle.

Preserve both properties. They are what keeps isolation an incremental change rather than a
rewrite.

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
- [ ] **Report the `pdf_save_document` leak to Artifex.** Live in current upstream, has a
      pure-C reproducer, not yet sent.
- [ ] Install `llvm-symbolizer` (Fedora `llvm`) so LSan suppressions resolve under
      libFuzzer. Without it libFuzzer must run with `-detect_leaks=0`.
- [ ] Give `fuzz_ops` far more time. At 133 executions per second it has had a fraction of
      `fuzz_open`'s exercise, on the code that does more with attacker-shaped structure.
- [ ] Decide when process isolation lands — M2 (viewer opens arbitrary files) is the
      natural forcing point
