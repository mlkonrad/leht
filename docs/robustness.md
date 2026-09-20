# Robustness and untrusted input

A PDF is an untrusted input. People open files that arrived by email, from a scanner they
don't control, or off a website. This file records what we know about how the engine
behaves on hostile input, and what the plan is.

## The one bug found so far — resolved

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
- A crash found in a dependency gets a pure-library reproducer before it is called
  upstream. "It crashes in our tool" is not a bug report.
- The mutation fuzzer is run by hand, not in CI, and the reason is written down in
  [CONTRIBUTING.md](../CONTRIBUTING.md). A fuzz target nobody runs is not protection.

## Outstanding

- [x] ~~Verify `obj<<` upstream~~ — fixed in 1.28.4; no report needed
- [ ] Install `libasan`/`libubsan` runtimes so `LEHT_SANITIZE=ON` actually links and runs
- [ ] Install `clang` for coverage-guided libFuzzer rather than blind mutation
- [ ] Decide when process isolation lands — M2 (viewer opens arbitrary files) is the
      natural forcing point
