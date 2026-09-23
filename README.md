# Leht

A fast, desktop-environment-neutral PDF toolkit for Linux.

**Leht** (Estonian: *sheet, page, leaf* — pronounced "leht") is a viewer, editor and
toolbox in one: view, merge, split, compress, annotate, redact, fill forms and sign —
locally, with no account and no upload. It looks correct on GNOME, KDE, XFCE and
everything else, because it favours no desktop.

> **Status: pre-alpha.** The engine, the `leht` command-line tool and a Qt6 viewer with
> editing (annotations, forms, true redaction) work.
> The viewer parses every document in a sandboxed worker process, so a malicious PDF costs
> a respawn, not the application. `LEHT_BUILD_UI` is still `OFF` by default. Expect the CLI
> surface to shift before 1.0.

## Why

Linux has no fast, complete, DE-neutral PDF tool. Okular is KDE-flavoured and heavy;
Evince/Papers is GNOME-flavoured and read-only in practice; anything involving real
editing means Acrobat under Wine or handing your document to a website. Leht is one
native toolkit that covers the operations people actually need.

Design commitments:

- **Local by default.** Documents never leave the machine. No account, no telemetry.
- **Structure-preserving.** Editing a page must not rewrite the parts you didn't touch.
  Merging images must not re-encode JPEGs that were already fine.
- **DE-neutral.** Qt6 Widgets, XDG portals for file dialogs and dark mode. No desktop's
  house style imposed on another's.
- **Core before pixels.** The engine is a plain C++20 library with zero Qt. The GUI is one
  consumer of it; the CLI is another; bindings are cheap later.
- **Fast on real files.** The benchmark corpus is the 300-page scanned contract, not a
  three-page invoice.

One honest limitation while this is pre-alpha: the parser is MuPDF. The viewer never runs
it — a sandboxed `leht-worker` per document does, and a crash there is contained — but
**the CLI parses in-process**, so a parser crash takes that one command with it. See
[docs/robustness.md](docs/robustness.md#process-isolation).

## Using the CLI

```
leht info      FILE...                            pages, size, metadata, encryption
leht render    FILE -o OUT.png [-p N] [-z Z]      render one page to PNG
leht merge     FILE... -o OUT.pdf                 merge PDFs and images, in order
leht split     FILE -o 'part-%03d.pdf' [-n N]     split into chunks
leht extract   FILE -p RANGES -o OUT.pdf          keep only these pages
leht remove    FILE -p RANGES -o OUT.pdf          drop these pages
leht rotate    FILE -p RANGES -d DEG -o OUT.pdf   rotate in place
leht compress  FILE -o OUT.pdf [--preset P] [-q N] [--linearize]
leht encrypt   FILE -o OUT.pdf [--user-pw PW] [--owner-pw PW] [--method M]
leht decrypt   FILE -o OUT.pdf [--password PW]
leht redact    FILE -o OUT.pdf [--text TERM] [--rect P:X0,Y0,X1,Y1]... [-p RANGES]
leht crop      FILE -o OUT.pdf (--box X0,Y0,X1,Y1 | --margins N[,T,R,B]) [-p RANGES]
leht watermark FILE -o OUT.pdf --text TEXT [--opacity F] [--angle DEG] [--under]
leht annots    FILE                               list annotations, with ids
leht annotate  FILE -o OUT.pdf [--highlight TEXT] [--note P:X,Y:TEXT] [--stamp P:NAME]...
leht form      FILE                               list form fields
leht fill      FILE -o OUT.pdf NAME=VALUE... [--flatten]
leht sign      FILE -o OUT.pdf --p12 ID.p12 [--box P:X0,Y0,X1,Y1] [--tsa URL]
leht verify    FILE [--trust CA.pem]... [--json]  check every signature
```

`-o` output · `-p` pages · `-z` zoom (1.0 = 72 DPI) · `-n` pages per file · `-d` degrees ·
`-q` JPEG quality · `--preset lossless|print|ebook|screen` (default `ebook`) ·
`--method aes256|aes128|rc4` (default `aes256`) · `--linearize`

**Redaction removes; it does not cover.** Text, image pixels and drawing under the box
leave the file, along with everything else that repeats them: overlapping annotations and
fields, thumbnails, marked-content `/ActualText`, the structure tree and earlier
revisions. Anywhere the term still appears (metadata, bookmarks) is listed, and `redact`
exits 3. **Cropping only hides.** `fill` never runs a document's JavaScript. Details and
how each claim is tested: [docs/editing.md](docs/editing.md).

**A signature signs a file, not a document.** Signing appends a revision and leaves the
original bytes untouched, so signatures already in the file stay valid — and so saving a
signed document now appends rather than rewrites. `verify` keeps four questions apart: are
these the signed bytes, whose key was it, is that name trustworthy, and was anything added
afterwards. Exit codes say the same: 4 broken, 5 untrusted, 6 changed after signing. The
private key never enters the sandboxed worker that parses the PDF, and the signature blobs
coming out of a document are only ever parsed inside it: [docs/signing.md](docs/signing.md).

**Page ranges are 1-based and inclusive:** `1-5,8,12-`. A **descending range reverses those
pages** — `-p 5-1` flips them. That is deliberate, not a parsing accident.

**`compress` and `encrypt` refuse to write over their input.** You always get both files,
so you can compare sizes and keep whichever you want. Destroying the original to find out
whether the result was worth it is not a workflow.

### Compression

Presets target an effective DPI: `screen` 72, `ebook` 150, `print` 300, plus `lossless`,
which only does object GC, stream de-duplication and recompression. A re-encoded image is
kept **only if it actually came out smaller**, and images carrying transparency (`/SMask`
or `/Mask`) are skipped entirely, since JPEG has no alpha channel.

The DPI estimate is a **conservative heuristic**: effective DPI is inferred from an image's
pixel dimensions against a nominal 11-inch page, *not* from its measured placement in the
content stream. It only ever shrinks toward the budget and never upsamples. One measured
example: a 466 KB merge of a 150 DPI scan plus text went to 106 KB at `--preset screen`,
77% smaller. Your files will differ.

## Scope

**Done**
- **M1 — core + CLI:** context, document model, renderer, page cache, and the ops layer
  behind the commands above; fuzz targets and sanitizer runs.
- **M2 — Qt6 viewer:** scroll, zoom, rotate, search, text selection, outline, thumbnails,
  encrypted documents, printing.
- **M3 — process isolation:** the viewer never parses a PDF. A fresh `leht-worker` per
  document does, under seccomp-bpf, rlimits and namespaces, and a crash in it is contained
  and reported. Design, threat model and measured cost:
  [docs/robustness.md](docs/robustness.md#process-isolation).

- **M4 — editing:** true redaction (content removed, not covered), crop, watermarks,
  annotations and form filling, in the CLI and in the viewer. The viewer edits through
  the sandboxed worker, with undo and redo, and a crash loses no edits. See
  [docs/editing.md](docs/editing.md).
- **M5 — signatures:** PAdES signing from a PKCS#12 key (B-B, or B-T with a timestamp
  authority), verification that keeps integrity, identity, trust and later changes apart,
  incremental saving so signatures survive, and a visible signature mark that says it is
  only a picture. The key stays out of the sandbox; the hostile DER stays inside it. See
  [docs/signing.md](docs/signing.md).

**Now** — release readiness: `cmake --install` with desktop integration, CI on every push,
and the whole suite, sanitizers and a fuzz pass run against the pinned MuPDF 1.28.4.

**Later** — packaging as Flatpak, RPM and DEB, once Leht has been used day to day.

**Explicitly out of scope for v1: in-place text editing.** It requires font matching
against subsetted embedded fonts plus line reflow, works only on simple documents, and
fails visibly on complex ones. If it ever ships it will be a labelled experimental mode,
never a headline promise.

Windows and macOS are not v1 targets. Qt6 keeps that door open at no extra cost today.

## Layout

| Path | What it is |
|---|---|
| `core/` | The engine: context, document, renderer, page cache, ops. **Never links Qt.** |
| `cli/` | `leht`, the headless command-line tool over the full core surface. |
| `ui/` | The Qt6 Widgets viewer. Parses nothing itself. Off by default at build time. |
| `worker/` | `leht-worker`: the sandboxed process that parses documents for the viewer. |
| `ipc/` | The viewer ⇄ worker wire format. Decodes as if the worker were hostile. |
| `crypto/` | Signing and verification: PKCS#12, CMS/PAdES, RFC 3161. **Links OpenSSL only, never MuPDF, and parses no PDF.** |
| `bench/` | Benchmark harness — timings against a real-world corpus. |
| `tests/corpus/` | Generated test PDFs. Unit tests live in `core/tests/`. |

`core/` links **MuPDF only** — MuPDF 1.28 covers merging (`pdf_graft_page`) and
compression, linearisation and AES-256 encryption (`pdf_write_options`), so there is no
second PDF library. MuPDF is linked `PRIVATE` and no public header names a MuPDF type,
only forward-declared opaque pointers. That keeps the render backend swappable and stops
consumers inheriting MuPDF's include path. Keep it that way.

Rendering is **one independent `leht::Context` per thread, never cloned contexts** — a
measured constraint, not a style choice. See [docs/threading.md](docs/threading.md).

## Building

Requires CMake 3.28+, Ninja, a C++20 compiler, MuPDF, OpenSSL 3.2+ and libseccomp (for
`leht-worker`). Qt6 only for the viewer.

**MuPDF 1.28.4 or newer is strongly recommended.** Older versions — including the 1.28.2
that Fedora 44 ships — abort the process on some malformed files. Leht still builds and
works against them, and the build warns; don't open untrusted PDFs on one. See
[docs/robustness.md](docs/robustness.md). `tools/build-mupdf.sh` downloads, checks and
builds the pinned 1.28.4 into `~/.cache/leht` and prints its path:

```sh
cmake -S . -B build -G Ninja -DLEHT_MUPDF_ROOT="$(tools/build-mupdf.sh)"
```

```sh
# Fedora
sudo dnf install gcc-c++ cmake ninja-build mupdf-devel openssl-devel libseccomp-devel
sudo dnf install qt6-qtbase-devel qt6-qtsvg  # only for the viewer

# Debian / Ubuntu
sudo apt install g++ cmake ninja-build libmupdf-dev libssl-dev libseccomp-dev
sudo apt install qt6-base-dev                # only for the viewer
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

Running the full test suite wants a few extra **command-line** tools that Leht itself never
needs at runtime — see [CONTRIBUTING.md](CONTRIBUTING.md).

CI (`.github/workflows/ci.yml`) builds and tests three ways on Fedora 44: against the
system MuPDF, against the pinned 1.28.4, and against 1.28.4 under ASan and UBSan with a
mutation-fuzz pass. `tools/ci-local.sh system|pinned|asan` runs the same job in a podman
container, from a read-only copy of the tree.

### Installing

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLEHT_BUILD_UI=ON \
      -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DLEHT_MUPDF_ROOT="$(tools/build-mupdf.sh)"
cmake --build build-release
cmake --install build-release
```

That installs `leht` and `leht-viewer` into `bin/`, `leht-worker` into `libexec/leht/`, and
a desktop entry and icon, so Leht appears in the application menu and in "Open with" for
PDFs. The viewer finds its worker relative to its own binary, so the tree can be moved.
To remove it: `xargs rm -v < build-release/install_manifest.txt`.

Build options — all default as shown:

| Option | Default | Effect |
|---|---|---|
| `LEHT_BUILD_CLI` | `ON` | Build the `leht` command-line tool |
| `LEHT_BUILD_UI` | `OFF` | Build the Qt6 viewer (requires `LEHT_BUILD_WORKER`) |
| `LEHT_BUILD_WORKER` | `ON` | Build `leht-worker`, the sandboxed parser process (needs libseccomp) |
| `LEHT_BUILD_TESTS` | `ON` | Build the test suite |
| `LEHT_BUILD_BENCH` | `ON` | Build the benchmark harness |
| `LEHT_SANITIZE` | `OFF` | AddressSanitizer + UBSan |

## Licence

**AGPL-3.0-or-later** — see [LICENSE](LICENSE). Leht is open source and free forever. That
choice is deliberate and it is what unlocks MuPDF as the engine; see
[docs/licensing.md](docs/licensing.md) for the dependency chain and what the licence
obliges.

## The name

*Leht* is Estonian for a sheet of paper, a page, and a leaf — all three at once, which is
about as close to "a PDF" as a natural word gets. See [docs/branding.md](docs/branding.md).
