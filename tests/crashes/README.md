# Engine defect artifacts

Reproducers for defects in the engine, kept next to the evidence for them.
Crash inputs here are **deliberately excluded from the normal test corpus** —
adding them to `tests/corpus/` would abort the test suite rather than fail it.

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


---

## `mupdf_save_leak_repro.c` — MuPDF leaks ~874 bytes per save

`pdf_save_document` leaks when `do_garbage >= 2`, in the object renumbering
path: `renumberobjs` → `renumberobj` → `pdf_copy_dict` → `pdf_new_dict`.

```sh
gcc -g -O0 -fsanitize=address mupdf_save_leak_repro.c -o probe /usr/lib64/libmupdf.so
./probe ../corpus/text_10p.pdf 3
```

It is precisely the garbage-collection level that triggers it:

| `do_garbage` | meaning | leaked |
|---|---|---|
| 0 | none | nothing |
| 1 | collect | nothing |
| 2 | collect + renumber | 642 bytes, 19 allocations |
| 3 | collect + renumber + de-duplicate | 874 bytes, 23 allocations |

It accumulates linearly — 1 save leaks 874 bytes, 10 leak 8,740, 50 leak 43,700 —
and it is present in **both** MuPDF 1.28.2 and 1.28.4, so it is long-standing
rather than a recent regression. Unlike the `obj<<` crash above, **this one is
still live in current upstream and is worth reporting to Artifex.**

### Root cause

In `renumberobjs()` (`source/pdf/pdf-write.c`), `renumberobj()` returns either the
object it was given (borrowed) or a modified copy from `pdf_copy_dict` /
`pdf_copy_array` (owned). When it returns a copy, the caller hands it on:

```c
nval = renumberobj(ctx, doc, opts, obj);
if (nval != obj)
    pdf_update_object(ctx, doc, num, nval);
nval = NULL;
```

`nval = NULL` is there to stop the function's `fz_always { pdf_drop_obj(nval) }`
from dropping it, which reads as though the hand-off transfers ownership. It does
not: both `pdf_update_object` and `pdf_set_trailer` take **their own** reference
with `pdf_keep_obj`. So the caller's reference to the copy is discarded without
ever being dropped. The same pattern appears three times — the trailer, the
general object case, and the indirect-reference case.

`nval` cannot simply stop being nulled, because when `renumberobj` returns its
input unchanged, `nval` is borrowed and dropping it would over-release.

This is exactly the bug class Leht had in its own code — `pdf_dict_put` with a
freshly created `pdf_new_int` — so it is an easy mistake to make against this API.

### Patch

[`mupdf-renumberobjs-leak.patch`](mupdf-renumberobjs-leak.patch), ten lines against
1.28.4: drop the reference after each hand-off, and only when it is owned. It
applies cleanly to the pristine 1.28.4 tarball (`patch -p1`).

Validated three ways against an ASan build of patched 1.28.4:

| check | unpatched | patched |
|---|---|---|
| reproducer, `do_garbage` 2 | 642 bytes leaked | clean |
| reproducer, `do_garbage` 3 | 874 bytes leaked | clean |
| 50 consecutive saves | 43,700 bytes leaked | clean |

And the stronger test: Leht's entire sanitized suite — 76 cases across 9
executables, heavy on merge, compress and encrypt, all of which save with
`do_garbage = 3` — run against the patched library **with `tests/lsan.supp`
disabled**. Zero leaks, zero ASan errors, zero UBSan errors. An over-release
would have shown up there as a use-after-free, and did not.

As a control, the same configuration against unpatched 1.28.4 leaks in
`test_compress` (874 bytes) and `test_encrypt` (14,166 bytes), so the harness
does detect this leak; "clean" is a real result, not a blind spot.

### Status

- Root cause identified and patch validated 2026-09-21.
- **Not yet reported.** MuPDF's README says to report on
  <https://bugs.ghostscript.com/> with *MuPDF* as the component and the relevant
  file attached.
- When upstream ships a fix, delete the `renumberobj` entries from
  `tests/lsan.supp` and re-run the sanitized suite.

### Why Leht keeps `do_garbage = 3` anyway

De-duplication is worth far more than 874 bytes. Merging four copies of one file
without it costs four copies of their shared fonts, which `test_merge` asserts.
For the CLI the leak is irrelevant — the process exits. It will matter for the
M2 viewer if it performs many saves in one session, and that is the point at
which to revisit it.

### Suppression

`tests/lsan.supp` suppresses this so a real leak in Leht's own code is not lost
in the noise. Two things about that file are load-bearing:

- Every entry must name an upstream defect with a reproducer here. **Never
  suppress a leak in our own code — fix it.**
- `ASAN_OPTIONS=fast_unwind_on_malloc=0` is required, not a tuning knob. LSan's
  default fast unwinder produces stacks too shallow to reach the frames the
  suppressions name, so without it every suppression silently fails to match
  and the suite goes red for no reason anyone can see.

### What ASan found in our own code

Worth recording, because the upstream leak was the *least* valuable thing this
exercise turned up. Running the suite under ASan/UBSan for the first time found
four real defects in Leht:

- `renderer.cpp` — `memcpy` with a null pointer when a degenerate page produced
  a pixmap with no samples. Undefined behaviour even at zero length.
- `ops/pages.cpp` — `pdf_dict_put(..., pdf_new_int(...))` in `rotate`. The
  dictionary takes its own reference, so the one `pdf_new_int` returns leaked.
- `ops/compress.cpp` — the same mistake three more times.
- `ops/merge.cpp` — an XObject dictionary that was never dropped.

All four are invisible without sanitizers, and all four are in code that passed
its functional tests.


---

## `mupdf_refchain_stackoverflow.py` — MuPDF stack overflow on deep reference chains

A PDF whose objects form a long indirect-reference chain (object 4 -> 5 -> 6 ->
...) crashes MuPDF with a stack overflow. `pdf_resolve_indirect` calls
`pdf_cache_object`, which resolves the next reference, which recurses again —
one frame per link. Around 200,000 links (a ~10 MB file) exhausts a default
8 MB stack.

```sh
python3 mupdf_refchain_stackoverflow.py chain.pdf 200000
leht compress chain.pdf -o /dev/null      # SIGSEGV
```

**Upstream, and unlike `obj<<` it is still live.** A pure-C program calling only
`pdf_save_document` crashes identically, and an ASan build of MuPDF **1.28.4**
(current upstream) reports:

```
ERROR: AddressSanitizer: stack-overflow
    #0 pdf_cache_object
    #1 pdf_resolve_indirect
```

No single object is deeply nested, so MuPDF's syntactic nesting cap (~128
levels, which does stop deeply-nested arrays and dicts) does not apply — the
depth is in the references.

### Severity, honestly

Lower than the earlier finds. It is a **stack overflow, i.e. a crash / denial of
service**, not the heap corruption `obj<<` was, and it needs a crafted file of
several megabytes rather than five bytes. Every recursive-descent PDF parser has
had a bug of this shape; it is a known class.

### What Leht can and cannot do

Leht cannot fix this from `core/`: the recursion is inside MuPDF, and MuPDF
exposes no depth limit to cap it. The only real defenses are (a) MuPDF growing an
internal limit, and (b) **process isolation**, so a parser crash on a hostile
file costs a worker process rather than the application. It is the strongest
argument yet for the isolation direction in
[../../docs/robustness.md](../../docs/robustness.md), because here there is no
in-process fix available at all.

Worth reporting to Artifex alongside the save leak. No artifact is committed —
the triggering file is multi-megabyte — so the generator stands in for it.
