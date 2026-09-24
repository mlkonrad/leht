# OCR

A scanned page is a picture: Find, selection, copying and `leht text` all come back
empty. OCR reads the words in the picture and lays them over it as an **invisible text
layer**. The page looks exactly as it did — a test renders it before and after and compares
every pixel — but its words can now be found, selected and copied.

```
leht ocr FILE -o OUT.pdf [-p RANGES] [--lang est+eng] [--dpi 300] [--force]
leht ocr --languages
```

In the viewer: **More → Recognize Text (OCR)…**, with the languages installed on the
machine, the pages, and the resolution; a progress dialog reads page by page and can be
cancelled.

## What it does, and does not

- **The picture is never touched.** The layer is text in render mode 3 (drawn with no
  ink), in a form XObject appended to the page, after the existing content has been
  wrapped in `q`…`Q` so nothing it left behind can shift the layer.
- **Each word covers its word.** It is placed on the word's box and stretched to its width,
  so a selection highlights the word in the picture, not a guess next to it. Rotated pages
  work: the box is as the page is displayed.
- **Any script.** The layer uses a glyphless font: every character maps to one glyph that
  draws nothing, and the font's ToUnicode map gives the character back. So Estonian õ ä ö ü
  š ž come out exactly — in Leht, and in `pdftotext`, which a test checks. The font is
  Tesseract's own `pdf.ttf` (572 bytes, Apache-2.0), embedded once per document and reused
  by every page after.
- **Pages that already have text are left alone** (16 or more characters), unless
  `--force` or the dialog says otherwise. A page made on a computer already has its text;
  reading it again would only add a second, worse copy.
- **Guesses are dropped.** Words Tesseract is less than 30% sure of are left out, so a speck
  of dust does not become searchable text; a blank page gives no words at all.
- **Languages**: Estonian and English by default when Estonian is installed, English
  otherwise. Any language Tesseract has data for can be named — `leht ocr --languages`
  lists them. A language that is not installed is refused by name rather than quietly
  dropped.

## Where it runs

Tesseract is a large C++ library reading images that came from an untrusted document, so
in the viewer it runs in its **own sandboxed process**, `leht-worker --ocr=LANGS`,
started for one OCR run and ended after it:

1. It loads Tesseract and every language's data **first** — that means opening files.
2. Then the same seccomp sandbox as the document worker goes up (no files, no network, no
   exec), with a larger memory limit.
3. Then it sees **pixels only**: the document worker renders each page, and the OCR worker
   gets the bitmap and returns words. It never sees the PDF, and accepts no file
   descriptors.

The order is not a detail. Loading a language after the sandbox is up is a file open, and
seccomp kills the process for it — the same trap as OpenSSL's lazy algorithm loading in
signing. A test starts the OCR worker with the load moved after the sandbox and checks that
it dies, so nobody "simplifies" the order back.

MuPDF has an OCR device of its own; it is not used, for exactly that reason: it starts
Tesseract lazily, inside whatever process is rendering. And its PDF-OCR writer rasterises
the whole page, throwing the vector content away.

**Undo and crash recovery never run OCR again.** The words go to the document worker as an
ordinary edit, which the viewer keeps in its edit log. Replaying the log — after an undo, or
into a fresh worker after a crash — writes the same words again, instantly and exactly. A
whole run is **one undo step**, however many pages it read; pages finished before a cancel
are kept.

The CLI runs Tesseract in-process, like everything else the CLI does (see
[robustness.md](robustness.md#process-isolation)).

## Building

Tesseract (`tesseract-devel`, `leptonica-devel`) is needed to build `ocr/`; its language
data (`tesseract-langpack-eng`, `tesseract-langpack-est`) to use it. `-DLEHT_WITH_OCR=OFF`
builds Leht without OCR: `leht ocr` and the viewer's menu entry then say so.

The pinned MuPDF is built with `HAVE_TESSERACT=no`: its bundled static Tesseract would
clash, symbol for symbol, with the system library Leht links.

## How it is tested

- `ocr/tests`: page 1 of the corpus rendered at 300 dpi must read as its words, with boxes
  within a few points of where the PDF says they are; an Estonian line must come back with
  its õ, š and ž; a blank page gives nothing.
- `core/tests/test_ocr_layer.cpp`: the layer is pixel-invisible, every word is found inside
  its box (also on a rotated page), Estonian letters survive `pdftotext`, the font is in the
  file once, and `qpdf --check` passes.
- The OCR worker reads under its sandbox, refuses anything but pixels, and dies if it loads
  late; a text-layer edit replays and is found by search.
- The CLI OCRs a page it first turned into a picture, and the viewer does the same through
  its dialog path, then undoes the run in one step.
- `fuzz_edit` writes a text layer of awkward words onto every fuzzed document.
