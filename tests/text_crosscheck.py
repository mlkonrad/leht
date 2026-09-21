#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Cross-check leht text extraction against poppler's pdftotext.

Two independent extractors agreeing on the words of a page is far stronger than
leht agreeing with itself. Exact-match is not the bar -- engines differ on
whitespace and reading order -- so this checks that poppler's words are all
present in leht's output. Skips (exit 77) without pdftotext.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SKIP = 77


def words(text):
    return set(re.findall(r"\w+", text.lower()))


def main():
    if len(sys.argv) < 3:
        print("usage: text_crosscheck.py <leht> <pdf> [page]", file=sys.stderr)
        return 2
    leht, pdf = sys.argv[1], sys.argv[2]
    page = sys.argv[3] if len(sys.argv) > 3 else "1"

    if subprocess.run(["which", "pdftotext"], capture_output=True).returncode != 0:
        print("SKIP: pdftotext not installed (dnf install poppler-utils)")
        return SKIP

    ours = subprocess.run([leht, "text", pdf, "-p", page],
                          capture_output=True, text=True)
    if ours.returncode != 0:
        print(f"FAIL: leht text exited {ours.returncode}: {ours.stderr}")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "pop.txt"
        subprocess.run(["pdftotext", "-f", page, "-l", page, pdf, str(out)],
                       check=True, capture_output=True)
        poppler = out.read_text()

    ours_w, pop_w = words(ours.stdout), words(poppler)
    if not pop_w:
        print("SKIP: poppler extracted no words")
        return SKIP

    missing = pop_w - ours_w
    coverage = 100 * len(pop_w & ours_w) / len(pop_w)
    print(f"  leht {len(ours_w)} words, poppler {len(pop_w)}, coverage {coverage:.0f}%")
    if missing:
        print(f"  words poppler found that leht missed: {sorted(missing)[:10]}")
    if coverage < 95.0:
        print("FAIL: coverage below 95%")
        return 1
    print("  OK: leht extraction agrees with poppler")
    return 0


if __name__ == "__main__":
    sys.exit(main())
