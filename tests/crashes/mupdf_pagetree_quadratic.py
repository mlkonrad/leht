#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Generate a PDF whose page lookups are quadratic in MuPDF.

The page tree's /Kids lists one object that never parses ("[1 0 R 2 R]") next
to N ordinary pages. MuPDF cannot build its page map for the tree, falls back
to walking it for every page lookup, and -- because a failed parse is never
cached -- re-parses the broken object on every walk. Anything that visits every
page (a viewer sizing pages on open, page removal, merging) goes quadratic:

    python3 mupdf_pagetree_quadratic.py slow.pdf 16000     # ~1.9 MB
    gcc -O2 pure_mupdf_pagetree_repro.c -o repro /usr/lib64/libmupdf.so
    ./repro slow.pdf                                       # ~10 s, vs ~0.1 s clean

Pass --clean to omit the broken object: the same document, loading in linear time.
Found by fuzz_ops as a libFuzzer timeout. See README.md.
"""
import sys


def build(path, n, clean=False):
    content = 4 + n
    objs = {1: b"<< /Type /Catalog /Pages 2 0 R >>"}
    kids = b" ".join(b"%d 0 R" % (4 + i) for i in range(n))
    bad = b"" if clean else b"3 0 R "
    objs[2] = b"<< /Type /Pages /Count %d /Kids [" % n + bad + kids + b"] >>"
    objs[3] = b"[1 0 R 2 R]"  # a syntax error MuPDF hits on every load
    for i in range(n):
        objs[4 + i] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
                       b" /Contents %d 0 R >>" % content)
    objs[content] = b"<< /Length 0 >>\nstream\n\nendstream"

    out, offs = bytearray(b"%PDF-1.7\n"), {}
    for k in sorted(objs):
        offs[k] = len(out)
        out += b"%d 0 obj\n" % k + objs[k] + b"\nendobj\n"
    xref, size = len(out), max(objs) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % size
    out += b"".join(b"%010d 00000 n \n" % offs[k] for k in range(1, size))
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (size, xref)
    with open(path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--clean"]
    if len(args) != 2:
        sys.exit(f"usage: {sys.argv[0]} OUT.pdf PAGES [--clean]")
    build(args[0], int(args[1]), clean="--clean" in sys.argv)
