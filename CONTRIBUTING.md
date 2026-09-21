# Contributing to Leht

## Set up

Build dependencies — CMake 3.28+, Ninja, a C++20 compiler, MuPDF, and libseccomp for
`leht-worker`, the sandboxed process the viewer parses documents in. That is the whole
list; `core/` links no second PDF library. A CLI-only build can drop libseccomp with
`-DLEHT_BUILD_WORKER=OFF`.

```sh
# Fedora
sudo dnf install gcc-c++ cmake ninja-build mupdf-devel libseccomp-devel
```

Building the viewer (`-DLEHT_BUILD_UI=ON`) additionally needs **`qt6-qtbase-devel`**. The
runtime `qt6-qtbase-gui` package is not enough — without the `-devel` package there is no
`Qt6Config.cmake` and `ui/` cannot be configured at all.

**Test-time dependencies are separate packages and none of them is a runtime dependency
of Leht:**

```sh
sudo dnf install mupdf qpdf ghostscript poppler-utils python3-pillow python3-numpy \
                 libasan libubsan clang
```

- `mupdf` and `qpdf` — the `mutool` and `qpdf` **command-line** binaries, which the
  `-devel` packages do not ship. `qpdf --check` is valuable here precisely *because* qpdf
  is not the library under test: it is an independent opinion on whether output is valid.
- `ghostscript` — used by the corpus generator only.
- `poppler-utils`, Pillow, NumPy — for the render cross-check described under Testing.
- `libasan`, `libubsan` — **required for `-DLEHT_SANITIZE=ON` to link.** Fedora ships the
  compiler-side `libasan.so` symlink without the runtime, so a sanitizer build configures
  and compiles and then fails at link with `cannot find libasan.so.8.0.0`. Until these are
  installed, the sanitizers are configured but never exercised.
- `clang` — for coverage-guided libFuzzer. Without it the fuzz target still builds as a
  standalone mutation driver, which is how the MuPDF bug was found.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

Options: `LEHT_BUILD_CLI`, `LEHT_BUILD_UI`, `LEHT_BUILD_TESTS`, `LEHT_BUILD_BENCH`,
`LEHT_SANITIZE` (AddressSanitizer + UBSan). Defaults are in the [README](README.md).

**Warnings are errors in Debug builds.** The flag set is `-Wall -Wextra -Wpedantic
-Wshadow -Wold-style-cast -Wconversion -Wsign-conversion`, and `-Werror` is on for Debug —
a warning fails the build. Fix it rather than silencing it.

## Testing

Tests are plain-assert executables registered with CTest. There is no test framework
dependency and we are not adding one; a test is a `main()` that asserts and returns
non-zero.

`ctest --test-dir build` also runs a **render cross-check** against poppler's `pdftoppm`
at 144 DPI, comparing geometry, ink coverage and mean pixel difference. It is the strongest
correctness signal in the suite: two independent PDF engines agreeing pixel-for-pixel is
hard to fake. It **skips cleanly with exit code 77** when `pdftoppm`, Pillow or NumPy are
missing, so a skip is not a pass — install the tools above before trusting a green run.

The test corpus is **generated, not committed** — run `./tests/corpus/generate.sh`, which
needs ghostscript. Generated PDFs are gitignored. Don't commit binary test files.

### Sanitizers

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLEHT_SANITIZE=ON
cmake --build build-asan && ctest --test-dir build-asan
```

Run this before sending anything that touches `core/`. The first time the suite was ever
run under ASan/UBSan it found **four real defects** — a `memcpy` from a null pointer, and
three leaked MuPDF object references — all in code whose functional tests were passing.
Sixty-two green tests said nothing was wrong. That is the whole argument for running them.

Two things about the setup are load-bearing rather than tuning:

- `tests/lsan.supp` suppresses one known upstream MuPDF leak so a real leak of ours is not
  buried in the noise. **Every entry must name an upstream defect with a reproducer under
  `tests/crashes/`. Never suppress a leak in our own code — fix it.**
- `ASAN_OPTIONS=fast_unwind_on_malloc=0` is **required**, and CMake sets it for you. LSan's
  default fast unwinder produces stacks too shallow to reach the frames a suppression
  names, so without it every suppression silently fails to match and the suite goes red
  with no visible explanation.

Coverage-guided fuzzing with clang also needs `llvm-symbolizer` (Fedora package `llvm`) for
suppressions to resolve. Without it, run libFuzzer with `-detect_leaks=0` and rely on the
GCC ASan build for leak checking.

## Fuzzing

`ctest` runs **`fuzz_corpus_replay`**, which replays every corpus file unmutated. That is a
regression guard, not fuzzing.

Real fuzzing is run **by hand**:

```sh
./build/fuzz/leht_fuzz_open tests/corpus 4000
```

The mutation fuzzer is deliberately **not** in the default suite. It is unbounded and
nondeterministic, and on MuPDF older than 1.28.4 it aborts within roughly 22 iterations on
an upstream bug that has since been fixed — wiring a test into CI that goes red for reasons
outside the change under test does not buy safety, it teaches people to ignore CI. The cost
of that choice is that nobody fuzzes unless they choose to: **a fuzz target that is not run
is not protection.** Run it when you touch parsing, the ops layer, or anything that reaches
`fz_open_document`.

Crashes go in `tests/crashes/`, never `tests/corpus/` — a corpus file that aborts turns the
suite red for reasons unrelated to the change under test. Before blaming a dependency,
reduce the input and write a reproducer that links only that library; see
[docs/robustness.md](docs/robustness.md).

## Troubleshooting

**`-lmupdf` disappears when hand-compiling on Fedora.** `mupdf.pc` emits a malformed `-L`
with an empty path, which swallows the `-lmupdf` that follows it, so
`gcc $(pkg-config --libs mupdf)` fails to link. `cmake/FindMuPDF.cmake` sidesteps this with
`find_library` and an absolute path. If you are compiling a one-off reproducer by hand, pass
`/usr/lib64/libmupdf.so` directly instead of using `pkg-config`.

## Four rules that are not negotiable

**1. `core/` never links Qt, and no public header may name a MuPDF type.** Public headers
use forward-declared opaque pointers only. This is what keeps the engine headlessly
testable, the CLI nearly free, and the render backend swappable — a backend change should
mean rewriting `core/src/*.cpp`, never the API or its consumers.

**2. `fz_try`/`fz_catch` appear in exactly one function**, in `core/src/error.cpp`.
Everything else goes through `leht::guarded()`. MuPDF's `fz_try`/`fz_catch` are
`setjmp`/`longjmp` macros, and a `longjmp` past a C++ stack frame **skips destructors** —
silent leaks and corruption. The contract of the lambda you pass to `guarded()` is that it
may hold **only trivially-destructible locals**. If you need an owning C++ object, construct
it outside the guarded region.

This failure is silent — a `longjmp` through a C++ frame skips destructors with no
diagnostic, so it leaks or corrupts rather than crashing anywhere near the cause. In review,
the tell is any `std::string`, `std::vector`, `std::unique_ptr` or other owning object
constructed **inside** a `guarded()` lambda.

**3. Never render in parallel with cloned MuPDF contexts.** Use one independent
`leht::Context` per thread. This is measured, not stylistic: clones share a lock set,
MuPDF takes `FZ_LOCK_ALLOC` around every allocation, and cloned rendering gets *slower*
with each thread added. See [docs/threading.md](docs/threading.md) for the numbers and the
command to reproduce them.

**4. A diagnostic pragma needs a comment saying why the warning is wrong here.** Warnings
are errors in Debug, and the way that protection gets defeated is not by arguing with it
but by quietly silencing it. `#pragma GCC diagnostic ignored` is sometimes legitimate —
MuPDF's C headers trip `-Wold-style-cast`, and `core/src/mupdf_c.hpp` suppresses that in
one place, with a comment. What is not legitimate is suppressing a warning about your own
code because it is inconvenient.

`-Wformat-nonliteral` in particular is almost never wrong. It exists to name CWE-134,
uncontrolled format strings, and Leht shipped exactly that bug underneath a pragma
silencing it: `ops::split` passed the caller's output pattern to `snprintf` as its format
string, so `leht split in.pdf -o '%s.pdf'` segfaulted and `%n` would have been an
arbitrary write. The pragma was added by the same change that introduced the bug. See
[docs/robustness.md](docs/robustness.md).

If you find yourself reaching for a pragma, first assume the compiler is right.

## Licensing of contributions

Leht is **AGPL-3.0-or-later** and contributions are accepted under those terms. New source
files carry `SPDX-License-Identifier: AGPL-3.0-or-later`. If you are adding a dependency,
check it against the chain in [docs/licensing.md](docs/licensing.md) first — anything
incompatible with AGPL-3.0, or any dependency that would make the render backend harder to
replace, needs discussion before the code lands.
