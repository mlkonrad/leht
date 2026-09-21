#!/usr/bin/env bash
# Generates the leht test corpus. Idempotent: existing files are left alone.
# Ghostscript is a build/test-time tool here, not a runtime dependency of leht.
set -euo pipefail
cd "$(dirname "$0")"

have_gs() { command -v gs >/dev/null 2>&1; }
if ! have_gs; then
    echo "error: ghostscript (gs) is required to generate the corpus" >&2
    exit 1
fi

# Text-heavy page: many short glyph runs, which is what stresses the shared
# glyph cache that FZ_LOCK_GLYPHCACHE protects. That is the contention we care
# about measuring, so the corpus has to actually exercise it.
emit_ps() {
    local pages="$1"
    cat <<PS
%!PS-Adobe-3.0
/Helvetica findfont 10 scalefont setfont
/lines 50 def
/drawpage {
  /pg exch def
  0 1 lines 1 sub {
    /i exch def
    54 750 i 14 mul sub moveto
    (leht corpus - page ) show pg 5 string cvs show
    ( - line ) show i 4 string cvs show
    ( - the quick brown fox jumps over the lazy dog 0123456789) show
  } for
  showpage
} def
1 1 ${pages} { drawpage } for
PS
}

build_text_pdf() {
    local name="$1" pages="$2"
    if [[ -f "${name}" ]]; then
        echo "  skip  ${name} (exists)"
        return
    fi
    emit_ps "${pages}" > "${name}.ps"
    gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=pdfwrite \
       -sOutputFile="${name}" "${name}.ps"
    rm -f "${name}.ps"
    echo "  made  ${name} (${pages} pages, $(du -h "${name}" | cut -f1))"
}

echo "generating corpus in $(pwd)"
build_text_pdf text_10p.pdf 10
build_text_pdf text_160p.pdf 160    # matches the upstream contention report
build_text_pdf text_500p.pdf 500    # peak-RSS target case

# Images for the merge path. The JPEG matters most: merging must embed its
# compressed stream verbatim rather than decoding and re-encoding it.
make_image() {
    local name="$1" device="$2" res="$3"
    if [[ -f "${name}" ]]; then
        echo "  skip  ${name} (exists)"
        return
    fi
    emit_ps 1 > "${name}.ps"
    gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE="${device}" -r"${res}" \
       -sOutputFile="${name}" "${name}.ps"
    rm -f "${name}.ps"
    echo "  made  ${name} ($(du -h "${name}" | cut -f1))"
}
make_image scan.jpg jpeg 150
make_image page.png png16m 96

# A deliberately non-page-shaped image, so tests can tell pages apart by
# geometry rather than by content.
if [[ ! -f wide.png ]]; then
    cat > wide.ps <<'WIDE'
%!PS-Adobe-3.0
/Helvetica findfont 24 scalefont setfont
0.2 0.4 0.8 setrgbcolor
20 60 moveto (leht wide) show
0 0 0 setrgbcolor
20 20 moveto (400x200 marker) show
showpage
WIDE
    gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=png16m -g400x200 \
       -sOutputFile=wide.png wide.ps
    rm -f wide.ps
    echo "  made  wide.png (400x200)"
fi

# Decompression bomb: a tiny file declaring a huge image. Kept in a
# subdirectory so corpus-wide consumers (the fuzz replay, libFuzzer seeding)
# never pick it up by accident; test_bombs names it explicitly.
mkdir -p bombs
if [[ ! -f bombs/image_16k.pdf ]]; then
    python3 - <<'BOMB'
import zlib
w = h = 16000
data = zlib.compress(b"\x00" * (w * 3 * 4), 9)   # 4 real rows; the rest implied
objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /XObject << /Im 4 0 R >> >> /Contents 5 0 R >>",
    b"<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB "
    b"/BitsPerComponent 8 /Filter /FlateDecode /Length %d >>\nstream\n" % (w, h, len(data))
    + data + b"\nendstream",
]
content = b"q 612 0 0 792 0 0 cm /Im Do Q"
objs.append(b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream")
out, offs = b"%PDF-1.7\n", []
for i, o in enumerate(objs, 1):
    offs.append(len(out)); out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
x = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
out += b"".join(b"%010d 00000 n \n" % o for o in offs)
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
open("bombs/image_16k.pdf", "wb").write(out)
BOMB
    echo "  made  bombs/image_16k.pdf ($(stat -c %s bombs/image_16k.pdf) bytes, declares 16000x16000)"
fi

# A deliberately malformed file: Document::open must throw, never crash.
if [[ ! -f damaged.pdf ]]; then
    printf '%%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\ntrailer\n' > damaged.pdf
    echo "  made  damaged.pdf (intentionally truncated)"
fi

echo "done"

# A PDF with a two-level outline (bookmarks), for the outline-extraction test.
# Written by hand because neither mutool nor ghostscript makes bookmarks easily,
# and doing it explicitly also documents the /Outlines structure leht parses.
if [[ ! -f outlined.pdf ]]; then
    python3 - <<'PY'
# 3 pages; outline: "Chapter One"->p1, "Chapter Two"->p2 with child "Section
# 2.1"->p3, "Chapter Three"->p3.
objs = {}
def add(n, body): objs[n] = body

add(1, b"<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R >>")
add(2, b"<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>")
for n, pg in [(3,0),(4,1),(5,2)]:
    add(n, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R >>")
add(6, b"<< /Length 44 >>\nstream\nBT /F1 12 Tf 72 720 Td (page) Tj ET\nendstream")
# Outline tree
add(10, b"<< /Type /Outlines /First 11 0 R /Last 13 0 R /Count 4 >>")
add(11, b"<< /Title (Chapter One) /Parent 10 0 R /Next 12 0 R /Dest [3 0 R /XYZ 0 792 0] >>")
add(12, b"<< /Title (Chapter Two) /Parent 10 0 R /Prev 11 0 R /Next 13 0 R "
        b"/First 14 0 R /Last 14 0 R /Count 1 /Dest [4 0 R /XYZ 0 792 0] >>")
add(13, b"<< /Title (Chapter Three) /Parent 10 0 R /Prev 12 0 R /Dest [5 0 R /XYZ 0 792 0] >>")
add(14, b"<< /Title (Section 2.1) /Parent 12 0 R /Dest [5 0 R /XYZ 0 400 0] >>")

out = b"%PDF-1.7\n"
offs = {}
for n in sorted(objs):
    offs[n] = len(out)
    out += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
xref = len(out)
maxn = max(objs)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (maxn + 1)
for n in range(1, maxn + 1):
    out += (b"%010d 00000 n \n" % offs[n]) if n in offs else b"0000000000 00000 f \n"
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (maxn + 1, xref)
open("outlined.pdf", "wb").write(out)
PY
    echo "  made  outlined.pdf (3 pages, 2-level outline)"
fi
