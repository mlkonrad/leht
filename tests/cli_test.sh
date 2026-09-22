#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# CLI behaviour tests: argument parsing and exit codes.
#
# The core/ tests exercise the library; nothing exercised the CLI itself, which
# is how `leht rotate -d abc` came to rotate by 0 degrees and exit 0. These
# check that mistakes at the command line are errors, with the exit code a
# script would see.
set -uo pipefail

LEHT="$1"
CORPUS="$2"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
IN="$CORPUS/text_10p.pdf"
failures=0

expect() {   # expect <want-exit> <description> <args...>
    local want="$1" desc="$2"; shift 2
    local got
    "$LEHT" "$@" >"$OUT/stdout" 2>"$OUT/stderr"
    got=$?
    if [[ "$got" == "$want" ]]; then
        printf '  ok  %s\n' "$desc"
    else
        printf 'FAIL  %s: expected exit %s, got %s\n' "$desc" "$want" "$got"
        sed 's/^/        /' "$OUT/stderr" | head -3
        failures=$((failures + 1))
    fi
}

# Non-numeric and malformed numbers must be errors, never silently 0.
expect 1 "rotate -d abc rejected"          rotate "$IN" -d abc -o "$OUT/a.pdf"
expect 1 "rotate -d 90xyz rejected"        rotate "$IN" -d 90xyz -o "$OUT/a.pdf"
expect 1 "rotate -d overflow rejected"     rotate "$IN" -d 99999999999 -o "$OUT/a.pdf"
expect 1 "compress -q banana rejected"     compress "$IN" -q banana -o "$OUT/a.pdf"
expect 1 "split -n empty rejected"         split "$IN" -n "" -o "$OUT/p-%d.pdf"
expect 1 "render -z nan rejected"          render "$IN" -z nan -o "$OUT/a.png"
expect 1 "render -z inf rejected"          render "$IN" -z inf -o "$OUT/a.png"
expect 1 "render -z 0 rejected"            render "$IN" -z 0 -o "$OUT/a.png"
expect 1 "render -z -3 rejected"           render "$IN" -z -3 -o "$OUT/a.png"
expect 1 "render -z 1e30 rejected"         render "$IN" -z 1e30 -o "$OUT/a.png"
expect 1 "render -p abc rejected"          render "$IN" -p abc -o "$OUT/a.png"

# Structural mistakes.
expect 2 "no arguments is a usage error"
expect 2 "unknown command is a usage error" frobnicate "$IN"
expect 1 "flag missing its value"          rotate "$IN" -o
expect 1 "missing output"                  compress "$IN"
expect 1 "missing input"                   info

# Dangerous split patterns (CWE-134 regression, at the CLI level).
expect 1 "split %s pattern rejected"       split "$IN" -o "$OUT/%s.pdf"
expect 1 "split %n pattern rejected"       split "$IN" -o "$OUT/%n.pdf"

# Text command.
expect 0 "text extract all pages"        text "$IN"
expect 0 "text extract one page"         text "$IN" -p 1
expect 0 "text search"                   text "$IN" --search line
expect 1 "text page out of range"        text "$IN" -p 999

# Redaction: a mistake must never quietly produce an unredacted file.
expect 1 "redact with nothing to redact"   redact "$IN" -o "$OUT/x.pdf"
expect 1 "redact bad --rect"               redact "$IN" --rect 1:1,2,3 -o "$OUT/x.pdf"
expect 1 "redact --rect without page"      redact "$IN" --rect 1,2,3,4 -o "$OUT/x.pdf"
expect 1 "redact --rect empty box"         redact "$IN" --rect 1:10,10,5,5 -o "$OUT/x.pdf"
expect 1 "redact --rect junk number"       redact "$IN" --rect 1:10,10,5x,50 -o "$OUT/x.pdf"
expect 1 "redact --rect page 0"            redact "$IN" --rect 0:0,0,9,9 -o "$OUT/x.pdf"
expect 1 "redact --rect page out of range" redact "$IN" --rect 99:0,0,9,9 -o "$OUT/x.pdf"
expect 1 "redact -p without --text"        redact "$IN" -p 1 --rect 1:0,0,9,9 -o "$OUT/x.pdf"
expect 1 "redact unknown --images"         redact "$IN" --text fox --images blur -o "$OUT/x.pdf"
expect 1 "redact a non-PDF"                redact "$CORPUS/page.png" --text x -o "$OUT/x.pdf"
[[ -e "$OUT/x.pdf" ]] && { echo "FAIL  a refused redact still wrote output"; failures=$((failures + 1)); }
expect 0 "redact --text"                   redact "$IN" --text "quick brown" -o "$OUT/r.pdf"
if command -v pdftotext >/dev/null && pdftotext "$OUT/r.pdf" - | grep -qi "quick brown"; then
    echo "FAIL  redacted text still extractable"; failures=$((failures + 1))
fi
expect 0 "redact --rect, twice"            redact "$IN" --rect 1:0,0,200,100 --rect 2:0,0,50,50 -o "$OUT/r.pdf"
expect 0 "redact -p limits --text"         redact "$IN" --text fox -p 2-3 --images remove --no-boxes -o "$OUT/r.pdf"

# Crop and watermark.
expect 1 "crop needs --box or --margins"   crop "$IN" -o "$OUT/c.pdf"
expect 1 "crop with both"                  crop "$IN" --box 0,0,9,9 --margins 5 -o "$OUT/c.pdf"
expect 1 "crop bad --box"                  crop "$IN" --box 0,0,9 -o "$OUT/c.pdf"
expect 1 "crop bad --margins"              crop "$IN" --margins 1,2 -o "$OUT/c.pdf"
expect 1 "crop negative margins"           crop "$IN" --margins -5 -o "$OUT/c.pdf"
expect 1 "crop box off the page"           crop "$IN" --box 5000,5000,6000,6000 -o "$OUT/c.pdf"
expect 0 "crop --box"                      crop "$IN" --box 36,36,400,500 -p 1-3 -o "$OUT/c.pdf"
expect 0 "crop --margins"                  crop "$IN" --margins 20,30,20,30 -o "$OUT/c.pdf"
expect 1 "watermark needs --text"          watermark "$IN" -o "$OUT/w.pdf"
expect 1 "watermark bad --opacity"         watermark "$IN" --text X --opacity 0 -o "$OUT/w.pdf"
expect 1 "watermark junk --angle"          watermark "$IN" --text X --angle 45deg -o "$OUT/w.pdf"
expect 1 "watermark bad --color"           watermark "$IN" --text X --color red -o "$OUT/w.pdf"
expect 1 "watermark non-Latin text"        watermark "$IN" --text "水印" -o "$OUT/w.pdf"
expect 0 "watermark"                       watermark "$IN" --text DRAFT -o "$OUT/w.pdf"
expect 0 "watermark all options"           watermark "$IN" --text "Copy 1" -p 2 --opacity 0.4 --angle -30 --size 48 --color '#cc0000' --under -o "$OUT/w.pdf"

# Valid invocations still succeed.
expect 0 "rotate -d 90"                    rotate "$IN" -d 90 -o "$OUT/r.pdf"
expect 0 "rotate -d -90"                   rotate "$IN" -d -90 -o "$OUT/r.pdf"
expect 0 "render -z 0.5"                   render "$IN" -z 0.5 -o "$OUT/r.png"
expect 0 "compress -q 50"                  compress "$IN" -q 50 -o "$OUT/c.pdf"
expect 0 "split %03d"                      split "$IN" -n 5 -o "$OUT/p-%03d.pdf"
expect 0 "--help"                          --help
expect 0 "--version"                       --version

if [[ $failures -gt 0 ]]; then
    printf '%d CLI test(s) failed\n' "$failures"
    exit 1
fi
