#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Cross-checks leht's renderer against poppler's pdftoppm.

Two independent rasterisers agreeing on geometry and ink coverage is far
stronger evidence of correctness than any self-comparison: leht's own tests can
only show it is consistent with itself. Antialiasing along glyph edges will
always differ, so the thresholds allow for that while still catching real
faults -- wrong scale, shifted origin, dropped glyphs, inverted colour.

Skips (exit 77, CTest's "not run") when pdftoppm, PIL or numpy are missing,
because these are test-time conveniences and not dependencies of leht.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

SKIP = 77

# A 144 DPI render is zoom 2.0 in leht's terms, since PDF user space is 72/inch.
DPI = 144
ZOOM = 2.0

MAX_MEAN_DIFF = 6.0      # out of 255
MAX_INK_DELTA = 0.5      # percentage points of page coverage
MAX_LOUD_PIXELS = 12.0   # percent of pixels differing by more than 16


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: render_crosscheck.py <leht-binary> <pdf> [page]", file=sys.stderr)
        return 2

    leht_bin, pdf = Path(sys.argv[1]), Path(sys.argv[2])
    page = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    try:
        import numpy as np
        from PIL import Image
    except ImportError as exc:
        print(f"SKIP: {exc}")
        return SKIP

    if subprocess.run(["which", "pdftoppm"], capture_output=True).returncode != 0:
        print("SKIP: pdftoppm not installed (dnf install poppler-utils)")
        return SKIP

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        ours = work / "leht.png"

        subprocess.run(
            [str(leht_bin), "render", str(pdf), "-p", str(page),
             "-z", str(ZOOM), "-o", str(ours)],
            check=True, capture_output=True)
        subprocess.run(
            ["pdftoppm", "-r", str(DPI), "-f", str(page), "-l", str(page),
             "-png", str(pdf), str(work / "poppler")],
            check=True, capture_output=True)

        rendered = sorted(work.glob("poppler-*.png"))
        if not rendered:
            print("FAIL: pdftoppm produced no output")
            return 1

        a = Image.open(ours).convert("RGB")
        b = Image.open(rendered[0]).convert("RGB")

        print(f"  leht     {a.size}")
        print(f"  poppler  {b.size}")
        if a.size != b.size:
            print("FAIL: geometry differs -- wrong scale or page box")
            return 1

        A = np.asarray(a, dtype=np.int16)
        B = np.asarray(b, dtype=np.int16)
        diff = np.abs(A - B)

        mean_diff = float(diff.mean())
        loud = float((diff.max(axis=2) > 16).mean() * 100.0)
        ink_a = float((A.min(axis=2) < 200).mean() * 100.0)
        ink_b = float((B.min(axis=2) < 200).mean() * 100.0)
        ink_delta = abs(ink_a - ink_b)

        print(f"  mean diff      {mean_diff:.3f} / 255   (limit {MAX_MEAN_DIFF})")
        print(f"  loud pixels    {loud:.3f}%          (limit {MAX_LOUD_PIXELS}%)")
        print(f"  ink coverage   {ink_a:.2f}% vs {ink_b:.2f}%  "
              f"(delta {ink_delta:.3f}, limit {MAX_INK_DELTA})")

        failures = []
        if mean_diff > MAX_MEAN_DIFF:
            failures.append(f"mean diff {mean_diff:.3f} > {MAX_MEAN_DIFF}")
        if loud > MAX_LOUD_PIXELS:
            failures.append(f"loud pixels {loud:.3f}% > {MAX_LOUD_PIXELS}%")
        if ink_delta > MAX_INK_DELTA:
            failures.append(f"ink delta {ink_delta:.3f} > {MAX_INK_DELTA}")

        if failures:
            print("FAIL: " + "; ".join(failures))
            return 1

        print("  OK: leht and poppler agree")
        return 0


if __name__ == "__main__":
    sys.exit(main())
