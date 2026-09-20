# Crash artifacts

Inputs that crash the engine. **These are deliberately excluded from the normal
test corpus** — adding them to `tests/corpus/` would abort the test suite rather
than fail it.

## `obj-dict-5byte.pdf` — MuPDF 1.28.2 heap corruption in xref repair

Five ASCII bytes, `obj<<`, abort MuPDF with `free(): invalid pointer`.

```sh
python3 -c "open('crash.pdf','wb').write(b'obj<<')"
gcc -O2 pure_mupdf_repro.c -o repro /usr/lib64/libmupdf.so
./repro crash.pdf
```

```
format error: cannot find version marker
warning: trying to repair broken xref
warning: repairing PDF document
free(): invalid pointer
Aborted (core dumped)
```

**This is upstream, not ours.** `pure_mupdf_repro.c` is 15 lines of plain C
containing no Leht code, and it calls nothing but `fz_open_document`. The stack
at the abort is entirely inside libmupdf:

```
fz_free
  ...
pdf_repair_xref_aux
pdf_open_document_with_stream
fz_open_accelerated_document
```

For contrast, poppler 26.01 rejects the same bytes cleanly:
`Syntax Error: End of file inside dictionary`, exit 0.

### Why it matters

An invalid free on attacker-controlled input is a memory-safety bug, not a
cosmetic one, and it is reachable from the most ordinary call in the library.
Any application that opens an untrusted PDF — which is every PDF viewer — can be
aborted by a five-byte file, with heap corruption as the mechanism.

### How it was found

`fuzz/fuzz_open.cpp` found it within 22 mutation iterations of the corpus, with
no sanitizers and no coverage guidance. Delta-debugging then reduced the
original 77,860-byte artifact to 5 bytes. That it fell out this fast suggests
the repair path is not heavily fuzzed upstream.

### Status: FIXED UPSTREAM in MuPDF 1.28.4. Do not report to Artifex.

Resolved 2026-09-20 by building MuPDF 1.28.4 from source and re-running the
same test.

| version | released | result on `obj<<` |
|---|---|---|
| 1.28.2 (what Fedora 44 ships) | 2026-08-04 | `free(): invalid pointer`, SIGABRT |
| 1.28.4 (upstream latest) | 2026-09-15 | `syntax error: invalid key in dict`, clean reject, exit 0 |

The same mutation campaign — identical corpus, identical seeding, one driver
compiled twice against the two libraries — separates them cleanly:

- **1.28.2 — crashed** with `free(): invalid pointer` inside 4,000 iterations
- **1.28.4 — survived** 4,000 iterations on the same seed, then a further
  60,000 across seeds 1, 7 and 99 without a single crash

1.28.4's changelog mentions "Fix double-free bug in PDF structure tree parsing",
which is the same bug class but a different code path from `pdf_repair_xref_aux`.
So the changelog does not describe this fix and we cannot point at the commit
that made it; the behaviour change is nonetheless unambiguous.

**There is nothing to report to Artifex.** The bug exists only in the version
Fedora ships, and upstream has already moved past it.

### What to do about it

`mupdf 1.28.4` or newer is the real minimum for handling untrusted input. The
build does not *require* it, because Fedora 44 ships 1.28.2 and demanding 1.28.4
would make Leht unbuildable on its own development platform. CMake emits a loud
warning instead — see the version check in `cmake/FindMuPDF.cmake`.

### What it means for Leht

Not a reason to abandon MuPDF: every C-language PDF parser has had bugs of this
class, and poppler and pdfium both carry long CVE histories. It does mean two
things.

1. Parsing untrusted input deserves process isolation eventually, so a parser
   crash costs a subprocess rather than the application. Chrome does exactly
   this with pdfium. `core/` is already structured for it — nothing in the
   public API exposes a MuPDF type, and batch work already uses independent
   `Context`s per worker rather than a shared one.
2. The fuzz target earns its place in CI. It found a real bug on its first run.
