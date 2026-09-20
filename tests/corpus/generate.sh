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

# A deliberately malformed file: Document::open must throw, never crash.
if [[ ! -f damaged.pdf ]]; then
    printf '%%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\ntrailer\n' > damaged.pdf
    echo "  made  damaged.pdf (intentionally truncated)"
fi

echo "done"
