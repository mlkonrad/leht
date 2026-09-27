# File tools: combine, reduce, split

Three whole-file jobs, on the command line and in the viewer's **Files** menu:

| Viewer | CLI | Engine |
|---|---|---|
| Combine Files… | `leht merge` | `ops::merge`, `ops::Merger` |
| Reduce File Size… | `leht compress` | `ops::compress` |
| Split Document… | `leht split`, `leht extract` | `ops::split`, `ops::extract` |

All three write **new files** and never change the one they read. The viewer and the CLI
produce the same bytes for the same options: the viewer is a front end to the same engine
calls, and a worker test compares the two.

## In the viewer

- **Combine Files** takes PDFs and images, in an order set by dragging or with Move
  Up/Down, and writes one PDF. The open document is listed first. Images become one page
  each, sized by their resolution, and a JPEG goes in as its own bytes, without being
  re-encoded.
- **Reduce File Size** offers the four presets in plain words. From best quality to
  smallest, they are Lossless, Print (about 300 dpi), Ebook (about 150 dpi) and Screen
  (about 72 dpi). It writes a copy under another name. If the copy would not be smaller,
  nothing is written and the dialog says so: a tool called Reduce must never hand back a
  bigger file.
- **Split Document** writes every N pages to a file, or one file per group of page ranges
  (`1-3; 4-10; 11-`). The files go into a chosen folder as `name-01.pdf`, `name-02.pdf`,
  and so on, zero-padded so they sort.

Each job runs with a progress dialog that can be cancelled.

## Unsaved edits, passwords and signatures

- **Unsaved edits.** Reduce and Split work on the file **as it is on disk**. With unsaved
  edits, they ask first to save or discard them. Combine asks the same when the open
  document is one of its inputs.
- **Encrypted documents.** The viewer asks for the password when the job starts. It does
  not reuse the one the document was opened with, because that lives in the render
  worker. A reduced copy keeps its encryption and passwords, since MuPDF's default is
  `PDF_ENCRYPT_KEEP`. Split parts are new documents and are **not** encrypted, as with
  `leht split`. Combine refuses a password-protected input: decrypt it first.
- **Signed documents.** Every one of these writes new bytes, so no output carries the
  document's signatures. The Reduce and Split dialogs say so when the document is signed.

## How it runs

Each job runs on a thread of its own (`ui/src/file_tools.cpp`), in a sandboxed
`leht-worker` of its own. It never runs in the worker that shows the document, for three
reasons:

- **Compression rewrites the document in place.** The worker that ran it closes the
  document afterwards (`on_compress`), and nothing else is asked of it.
- **A merge reads files the user never opened.** A hostile one should cost that job, not
  the open document.
- **A long compression should not stall rendering.**

The worker cannot open paths. So the viewer opens every input and every output and passes
descriptors, one per message (protocol version 9):

| Request (fd) | Reply | |
|---|---|---|
| `Compress` (output) | `Compressed` | after `Open`/`Authenticate` |
| `ExtractPages` (output) | `PagesWritten` | once per split part |
| `MergeBegin` | `MergeAdded{0}` | |
| `MergeAdd` (**input**) | `MergeAdded` | once per input, in order |
| `MergeFinish` (output) | `Merged` | |

A merge of any number of files never holds more than one input descriptor open. The
sandbox allows only 16 (`RLIMIT_NOFILE`), so `Merger::add_fd` takes ownership of each
input and closes it as soon as its pages are grafted in. A worker test merges 22 inputs to
prove it. Inputs are recognised by content, never by name: `%PDF-` in the first KiB means a
PDF (as readers allow), and anything else must be an image MuPDF can decode. A refused
input is an ordinary `Failed`, and the merge can carry on.

Outputs go into a temporary file beside the target, and are fsynced and renamed into place
only when the job succeeds (`Beside`, in `ui/src/worker_files.cpp`). A failed or cancelled
job leaves no half-written file. A split commits each part as it is written, as
`leht split` does. If it fails part-way, it says which parts it had already written.

## Not yet

- Thumbnails and per-file page ranges in Combine, and a size estimate before Reduce runs.
  These are in the UX plan.
- Choosing JPEG quality or the image-size limit in the viewer. The CLI has `-q`, and the
  protocol carries both.
