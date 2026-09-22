# Editing

M4 adds editing: redaction, crop, watermark, annotations and form filling. So far this is
the engine and CLI half (M4a). The viewer's editing tools, which reach these operations
through the sandboxed worker, are M4b.

Every operation acts on an **open document** and leaves saving to a separate step:

```
CLI:     Document::open(path)  → edits → doc.save(output)
Worker:  the document it holds → edits → doc.save_fd(fd the viewer passed)
```

So the sandboxed worker, which cannot open paths, runs exactly the code the CLI tests
exercise, and several edits compose into one save.

## Saving

- **Always a full rewrite**, never an incremental update. Incremental saving arrives with M5,
  where appending a revision is what keeps an existing signature valid.
- **Atomic.** `save()` writes a temporary file beside the target, fsyncs it and renames it
  over the target, then fsyncs the directory. A failed save leaves the target untouched.
  Saving over the file the document was opened from is safe, because the open document
  keeps reading the old inode.
- **Keeps the target's permissions**, or gives a new file what `open(2)` would under the
  process umask.
- **Garbage collection level 1 by default** (collect, no renumber). Levels 2 and 3 renumber
  the objects of the *open* document, not just the file written. That would silently change
  every annotation id the caller holds. Use them only for a final save.
- `save_fd()` writes into a descriptor it does not own, using only `write` and `lseek`,
  which the worker's seccomp policy already allows.

## Redaction

`leht redact FILE -o OUT --text TERM` or `--rect PAGE:X0,Y0,X1,Y1` (points from the page's
top-left, as displayed).

Redaction means **removal**. The most common way redaction fails is a black box drawn over
text that stays in the file. Leht takes the content out:

| What | How |
|---|---|
| Text, image pixels and line art under a box | `pdf_redact_page`: removed from the content stream. Images default to blacking out only the covered pixels. |
| Annotations and form fields overlapping a box, and replies to them | Deleted. Their `/Contents`, values and appearances can repeat the text. |
| The page's `/Thumb` | Deleted. It is a picture of the page before redaction. |
| `/ActualText`, `/Alt`, `/E` on marked content | Stripped from every marked-content sequence on the page, including inline `BDC` dictionaries. |
| The structure tree | Dropped for the whole document: its `/Alt` and `/ActualText` repeat page text. Tagged-PDF accessibility is lost; that is the cost of the redaction being real. |
| Earlier revisions and the original content streams | A redacted document always garbage-collects on save, whatever the options say. |

There is **no option to keep text** under a box. MuPDF's `REMOVE_UNLESS_INVISIBLE` image
mode is not offered either, because MuPDF's own header says that it leaks.

**What is reported rather than removed.** `redact_text` then searches the rest of the
document for the term: document metadata (Info and XMP), bookmarks, and, on the pages
searched, annotations and field values no box covered, plus text the redaction could not
reach (inside a pattern or a Type 3 glyph). It lists every hit. The CLI prints them and
**exits 3**, so a script cannot mistake "written, but not clean" for success. Rewriting a
title or deleting a bookmark is the user's decision, not a side effect.

**How it is verified.** A hand-built fixture hides one secret in every place above: page
text, an earlier incremental revision, an annotation and a reply, a form field, the
structure tree, inline `/ActualText` and a thumbnail. After `redact_text` the test checks
the result three independent ways:

1. Our own text search finds nothing.
2. Poppler's `pdftotext` finds nothing.
3. **The secret appears in no byte of the file.** This is checked on an uncompressed save,
   and again after `qpdf --qdf` has decompressed every stream.

The byte-level check matters most, and it proved its worth on the first run.

### MuPDF gap: inline `/ActualText`

MuPDF 1.28's redaction edits `/Alt` and `/ActualText` only on structure elements it
reaches through `/MCID`. An inline property dictionary in the content stream, as in
`/Span <</ActualText (secret)>> BDC ... EMC`, was written back untouched, still holding the
text whose glyphs had just been removed. The byte-level test caught it. Leht now re-emits
each redacted page, and its form XObjects, through MuPDF's own content interpreter with
those keys stripped from every `BDC`.

## Crop

`leht crop FILE -o OUT --box X0,Y0,X1,Y1` or `--margins N` / `--margins L,T,R,B`.

**Cropping hides; it does not remove.** It sets `/CropBox`, and everything outside it stays
in the file, one "show full page" away in most editors. The CLI says so on every run. Use
`redact` to take content out. The box is given as the page is displayed, so it follows
`/Rotate`, and it is clipped to the media box.

## Watermark

`leht watermark FILE -o OUT --text TEXT [--opacity F] [--angle DEG] [--size PT]
[--color RRGGBB] [--under]`

- It is **page content**, not a Watermark annotation, which viewers let the reader hide
  with one toggle.
- The mark is a Form XObject whose `/Matrix` maps display coordinates back to page space,
  so it is centred and angled as displayed, rotated pages included.
- The page's existing content is wrapped in `q`/`Q` first, so a graphics state it leaves
  behind (a stray `cm`, a colour) cannot skew or recolour the mark. The XObject's resource
  name is checked against the page's own resources.
- **No font is embedded.** The content stream references non-embedded base-14 Helvetica
  in WinAnsi encoding, and opacity goes through an ExtGState. MuPDF's PDF device would
  have embedded a 33 KB font for one word; this way the file grows by under 1 KB. The
  price is that text is limited to Windows-1252, and anything outside it is refused
  rather than drawn as boxes.

## Annotations

`leht annots FILE` lists them with ids. `leht annotate FILE -o OUT` accepts
`--highlight|--underline|--strike TEXT` (every occurrence, limited by `-p`),
`--note P:X,Y:TEXT`, `--stamp P:NAME[:BOX]`, `--delete ID`, `--author NAME` and
`--color RRGGBB`.

The engine creates highlight, underline, strike-out, squiggly, note, free text, ink,
square, circle and stamp annotations. **Every one gets an appearance stream**, so poppler
draws it the same as MuPDF does, and a reader that would not synthesise one itself still
shows it. An annotation's id is its PDF object number, and it stays stable while the
document is open (see Saving). Form widgets are not listed as annotations: forms own them.

## Forms

`leht form FILE` lists fields with their types, values, options and flags.
`leht fill FILE -o OUT NAME=VALUE... [--flatten]`.

- Covers text, checkbox, radio and choice fields, with hierarchical names
  (`address.street`) and export values (choosing "Finland" stores `fi`). Checkboxes also
  accept yes/no, on/off, true/false and 1/0.
- Values are checked before anything changes: max length, on-state names, options, and
  the read-only flag. Push buttons and signature fields are refused.
- **Document JavaScript never runs.** Keystroke, validate and calculate actions are
  ignored, and JavaScript is never enabled on any document. A form must not be able to
  execute code. The trade-off is that a field whose document relies on a script to
  reformat it will not be reformatted.
- **XFA-only forms are refused.** Their fields live in XML that only Adobe readers
  interpret, and reporting "no fields" would be wrong. For a **hybrid** form, the first
  change drops the XFA copy, which would otherwise be stale and would be what XFA-first
  readers show.
- `--flatten` bakes fields into the page content, and they can no longer be edited.

### MuPDF gap: button values stored as strings

MuPDF 1.28 stores a checkbox's or radio group's `/V` as a string (`(Yes)`). The spec
requires a name (`/Yes`), matching the widgets' `/AS`, and other readers and form-data
exports compare the two. qpdf's JSON view of a filled form caught it. Leht re-stores the
value as a name after MuPDF sets it.
