# Licensing

**Leht ships under AGPL-3.0-or-later.** Decided before any code was written, deliberately
and with the tradeoff on the table. This file records the reasoning so it doesn't get
re-litigated every time someone new reads `core/CMakeLists.txt` and notices MuPDF.

## The decision

Business model: **open source, free forever.** That made AGPL-3.0 acceptable, which in turn
unlocked MuPDF as the rendering and document engine. The two are linked — the licence is
not an accident of a dependency, the dependency is a consequence of the licence.

The accepted downside: some corporate legal teams auto-reject AGPL software. That is a
known, weighed cost of the model, not an oversight.

## The dependency chain

| Component | Licence | Role |
|---|---|---|
| **MuPDF** | AGPL-3.0 (Artifex; commercial licence also sold) | Everything PDF: render, text, merge, compress, linearise, AES-256, annotations, redaction, forms, and the signature dictionary's placement (the cryptography is OpenSSL's) |
| **Qt6** | LGPL-3.0, dynamically linked | UI toolkit for the viewer |
| **libseccomp** | LGPL-2.1-only, dynamically linked | Builds the seccomp-bpf filter that sandboxes `leht-worker` (M3) |
| **OpenSSL** (libcrypto, libssl) | Apache-2.0, dynamically linked | Signing and verification: PKCS#12, CMS/PAdES, RFC 3161 timestamps, and TLS for an https timestamp authority (M5) |
| **Tesseract** | Apache-2.0 | OCR, later phase |

**qpdf was removed from the build.** MuPDF 1.28 covers the structure work it was brought in
for — `pdf_graft_page` for collision-safe merging, `pdf_write_options` for garbage
collection, de-duplication, linearisation and AES-256. The qpdf and Ghostscript
*command-line* tools are used at test time only and are not linked, not shipped, and not
runtime dependencies, so their licences do not enter the chain.

AGPL-3.0 + LGPL-3.0 and LGPL-2.1 (both dynamic) + Apache-2.0 → **the app ships AGPL-3.0**. Compatible and
consistent, and simpler than it was: one copyleft library, not two.

OpenSSL 3 is Apache-2.0, which is GPLv3-compatible, so it raises nothing new — unlike
OpenSSL 1.x, whose old licence needed an exception clause. Nothing in `crypto/` is derived
from OpenSSL's code. MuPDF's own PKCS#7 helpers are not used (Fedora does not build them);
the CMS work is ours, over libcrypto.

Artifex's terms are strict and worth stating plainly: link MuPDF into your software and
*the entirety of that software* must be AGPL; offer it as a service and the entire
installation must be too. Leht satisfies this by being AGPL throughout. Anyone forking Leht
into a proprietary product cannot — they would need a commercial licence from Artifex
(sales@artifex.com).

Qt6 must stay **dynamically linked**. Static linking pulls Qt's LGPL relinking obligations
into the picture and complicates redistribution for no benefit.

If a Ghostscript subprocess is ever added for aggressive lossy compression, it must
**degrade gracefully when absent** — the in-process MuPDF path stays the default. It is
AGPL-3.0 too, so it would not change the outcome, but a hard dependency on an external
binary would change packaging.

## The architectural hedge

Independently of the licence, `core/` keeps MuPDF at arm's length: linked `PRIVATE`, and no
public header names a MuPDF type — only forward-declared opaque pointers. Swapping the
render backend (PDFium is BSD-3-Clause, if the calculus ever changes) would mean rewriting
`core/src/*.cpp`, not the public API or its consumers.

This is good engineering on its own terms — it keeps `core/` testable and the CLI and UI
insulated — and it happens to preserve an exit. Hold the line regardless.

## Outstanding

- [ ] Third-party notices file listing MuPDF, Qt and (later) Tesseract with their terms
- [ ] `SPDX-License-Identifier: AGPL-3.0-or-later` headers in source files
- [ ] AGPL §13 network-use clause: revisit if anything server-side is ever offered
- [ ] Flatpak/RPM/DEB metadata carries the correct licence field at M6
