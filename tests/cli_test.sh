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

# Annotations.
expect 1 "annotate with nothing to do"     annotate "$IN" -o "$OUT/n.pdf"
expect 1 "annotate bad --note"             annotate "$IN" --note "1:100:hi" -o "$OUT/n.pdf"
expect 1 "annotate --note no page"         annotate "$IN" --note "100,100:hi" -o "$OUT/n.pdf"
expect 1 "annotate --note page 0"          annotate "$IN" --note "0:1,1:hi" -o "$OUT/n.pdf"
expect 1 "annotate unknown stamp"          annotate "$IN" --stamp 1:Bogus -o "$OUT/n.pdf"
expect 1 "annotate stamp page too high"    annotate "$IN" --stamp 99:Draft -o "$OUT/n.pdf"
expect 1 "annotate --delete junk"          annotate "$IN" --delete 12x -o "$OUT/n.pdf"
expect 1 "annotate --delete missing id"    annotate "$IN" --delete 99999 -o "$OUT/n.pdf"
expect 1 "annotate empty --highlight"      annotate "$IN" --highlight "" -o "$OUT/n.pdf"
expect 0 "annotate everything"             annotate "$IN" --highlight fox --underline lazy -p 1 --note "2:72,72:Check: this" --stamp 1:Approved --stamp "3:Draft:100,100,300,160" --author Tester --color 00aa00 -o "$OUT/n.pdf"
expect 0 "annots lists them"               annots "$OUT/n.pdf"
if ! grep -q "Check: this" "$OUT/stdout"; then
    echo "FAIL  annots did not list the note"; failures=$((failures + 1))
fi
note_id=$(awk '$3 == "Text" { print $1 }' "$OUT/stdout")
expect 0 "annotate --delete by id"         annotate "$OUT/n.pdf" --delete "$note_id" -o "$OUT/n2.pdf"
expect 0 "annots after delete"             annots "$OUT/n2.pdf"
if grep -q "Check: this" "$OUT/stdout"; then
    echo "FAIL  deleted note is still listed"; failures=$((failures + 1))
fi
# Free text, moving and new words (M1).
expect 1 "annotate --freetext without a box" annotate "$IN" --freetext "1:hello" -o "$OUT/ft.pdf"
expect 1 "annotate --move junk"            annotate "$IN" --move "12:1,2,3" -o "$OUT/ft.pdf"
expect 1 "annotate --move missing id"      annotate "$IN" --move "99999:1,2,30,40" -o "$OUT/ft.pdf"
expect 1 "annotate --set-text no colon"    annotate "$IN" --set-text "12" -o "$OUT/ft.pdf"
expect 0 "annotate --freetext"             annotate "$IN" --freetext "1:100,60,260,100:Hello: Leht" --size 14 -o "$OUT/ft.pdf"
ft_id=$("$LEHT" annots "$OUT/ft.pdf" | awk '$3 == "FreeText" {print $1; exit}')
expect 0 "annotate --move and --set-text"  annotate "$OUT/ft.pdf" --move "$ft_id:300,80,500,140" --set-text "$ft_id:Moved" -o "$OUT/ft2.pdf"
"$LEHT" annots "$OUT/ft2.pdf" | grep -q "FreeText *Moved" || {
    echo "FAIL  the free text was not rewritten"; failures=$((failures + 1)); }
hl_id=$("$LEHT" annots "$OUT/n.pdf" | awk '$3 == "Highlight" {print $1; exit}')
expect 1 "a highlight cannot move"         annotate "$OUT/n.pdf" --move "$hl_id:1,1,50,20" -o "$OUT/ft3.pdf"
[[ -e "$OUT/ft3.pdf" ]] && { echo "FAIL  a refused move still wrote output"; failures=$((failures + 1)); }

# OCR (M2): a picture of a page becomes searchable.
if "$LEHT" ocr --languages 2>/dev/null | grep -qx eng; then
    expect 0 "render a page as a picture"      render "$IN" -p 1 -z 2 -o "$OUT/scan.png"
    expect 0 "the picture as a PDF"            merge "$OUT/scan.png" -o "$OUT/scan.pdf"
    # Through a file, not a pipe: grep -q stops reading at the first match,
    # and under pipefail leht's SIGPIPE would then fail the whole line.
    "$LEHT" text "$OUT/scan.pdf" > "$OUT/scan.txt"
    grep -q "quick" "$OUT/scan.txt" && {
        echo "FAIL  the scan already had text"; failures=$((failures + 1)); }
    expect 1 "ocr with a language not installed" ocr "$OUT/scan.pdf" --lang klingon -o "$OUT/nope.pdf"
    expect 1 "ocr --dpi out of range"          ocr "$OUT/scan.pdf" --dpi 5000 -o "$OUT/nope.pdf"
    expect 1 "ocr will not overwrite its input" ocr "$OUT/scan.pdf" -o "$OUT/scan.pdf"
    [[ -e "$OUT/nope.pdf" ]] && { echo "FAIL  a refused ocr still wrote output"; failures=$((failures + 1)); }
    expect 0 "ocr the scan"                    ocr "$OUT/scan.pdf" --lang eng --dpi 150 -o "$OUT/scan-ocr.pdf"
    "$LEHT" text "$OUT/scan-ocr.pdf" > "$OUT/scan-ocr.txt"
    grep -q "quick brown" "$OUT/scan-ocr.txt" || {
        echo "FAIL  the OCR'd scan does not say 'quick brown'"; failures=$((failures + 1)); }
    expect 0 "ocr leaves a page that has text"  ocr "$IN" -p 1 --lang eng -o "$OUT/digital.pdf"
    grep -q "already had text" "$OUT/stdout" || {
        echo "FAIL  ocr did not say it skipped a page with text"; failures=$((failures + 1)); }
else
    echo "  skip  ocr (no Tesseract English data, or built without OCR)"
fi

# Forms.
FORM="$CORPUS/form.pdf"
if [[ -f "$FORM" ]]; then
    expect 0 "form lists fields"               form "$FORM"
    grep -q "^name " "$OUT/stdout" || { echo "FAIL  form did not list 'name'"; failures=$((failures + 1)); }
    expect 1 "fill with nothing to do"         fill "$FORM" -o "$OUT/f.pdf"
    expect 1 "fill without ="                  fill "$FORM" name -o "$OUT/f.pdf"
    expect 1 "fill unknown field"              fill "$FORM" nope=1 -o "$OUT/f.pdf"
    expect 1 "fill bad checkbox value"         fill "$FORM" agree=maybe -o "$OUT/f.pdf"
    expect 1 "fill over the length limit"      fill "$FORM" name=abcdefghijklmnopqrstuvwxyz -o "$OUT/f.pdf"
    [[ -e "$OUT/f.pdf" ]] && { echo "FAIL  a refused fill still wrote output"; failures=$((failures + 1)); }
    expect 0 "fill two fields"                 fill "$FORM" name=Marlon "agree=yes" -o "$OUT/f.pdf"
    expect 0 "form shows the values"           form "$OUT/f.pdf"
    grep -q '"Marlon"' "$OUT/stdout" || { echo "FAIL  filled value not listed"; failures=$((failures + 1)); }
    expect 0 "fill --flatten"                  fill "$OUT/f.pdf" --flatten -o "$OUT/flat.pdf"
    expect 0 "flattened form has no fields"    form "$OUT/flat.pdf"
    grep -q "no form fields" "$OUT/stdout" || { echo "FAIL  flatten left fields"; failures=$((failures + 1)); }
else
    echo "  skip  forms (run tests/corpus/generate.sh for form.pdf)"
fi
expect 0 "form on a file without one"      form "$IN"


# Signing and verification. The PKI is made here and thrown away with $OUT; the
# deeper crypto tests live in crypto/tests. Skipped without openssl(1).
if command -v openssl >/dev/null 2>&1; then
    openssl req -x509 -newkey rsa:2048 -keyout "$OUT/ca.key" -out "$OUT/ca.pem" -days 2 \
        -nodes -subj "/C=EE/O=Leht CLI test/CN=Test Root" >/dev/null 2>&1
    openssl req -newkey rsa:2048 -keyout "$OUT/s.key" -out "$OUT/s.csr" -nodes \
        -subj "/C=EE/CN=Test Signer" >/dev/null 2>&1
    openssl x509 -req -in "$OUT/s.csr" -CA "$OUT/ca.pem" -CAkey "$OUT/ca.key" \
        -out "$OUT/s.pem" -days 1 >/dev/null 2>&1
    openssl pkcs12 -export -out "$OUT/id.p12" -inkey "$OUT/s.key" -in "$OUT/s.pem" \
        -certfile "$OUT/ca.pem" -passout pass:secret >/dev/null 2>&1
    printf 'secret\n' > "$OUT/pw"
    printf 'wrong\n' > "$OUT/badpw"

    expect 1 "sign without --p12"            sign "$IN" -o "$OUT/sig.pdf"
    expect 1 "sign with the wrong password"  sign "$IN" -o "$OUT/sig.pdf" --p12 "$OUT/id.p12" --password-fd 0 < "$OUT/badpw"
    expect 1 "sign with a missing key file"  sign "$IN" -o "$OUT/sig.pdf" --p12 "$OUT/nope.p12" --password-fd 0 < "$OUT/pw"
    expect 1 "sign --image without a box"    sign "$IN" -o "$OUT/sig.pdf" --p12 "$OUT/id.p12" --image "$CORPUS/page.png" --password-fd 0 < "$OUT/pw"
    [[ -e "$OUT/sig.pdf" ]] && { echo "FAIL  a refused sign still wrote output"; failures=$((failures + 1)); }

    expect 0 "sign, invisible"               sign "$IN" -o "$OUT/sig.pdf" --p12 "$OUT/id.p12" --password-fd 0 < "$OUT/pw"
    expect 1 "sign refuses to overwrite its input" sign "$OUT/sig.pdf" -o "$OUT/sig.pdf" --p12 "$OUT/id.p12" --password-fd 0 < "$OUT/pw"
    # An untrusted signer is not a broken signature, and says so with its own code.
    expect 5 "verify: intact but untrusted"  verify "$OUT/sig.pdf"
    grep -q "intact" "$OUT/stdout" || { echo "FAIL  verify did not report an intact signature"; failures=$((failures + 1)); }
    expect 0 "verify --trust: trusted"       verify "$OUT/sig.pdf" --trust "$OUT/ca.pem"
    expect 0 "verify --json"                 verify "$OUT/sig.pdf" --trust "$OUT/ca.pem" --json
    grep -q '"intact":true' "$OUT/stdout" || { echo "FAIL  verify --json missing intact"; failures=$((failures + 1)); }
    expect 0 "verify a file with no signatures" verify "$IN"
    grep -q "no signatures" "$OUT/stdout" || { echo "FAIL  verify was not quiet about an unsigned file"; failures=$((failures + 1)); }

    # The signed file keeps the original bytes: an incremental update.
    head -c "$(stat -c%s "$IN")" "$OUT/sig.pdf" | cmp -s - "$IN" || {
        echo "FAIL  signing did not keep the original bytes"; failures=$((failures + 1)); }

    # A second signature, and a visible one at that, leaves the first valid.
    expect 0 "sign again, visible"           sign "$OUT/sig.pdf" -o "$OUT/sig2.pdf" --p12 "$OUT/id.p12" --box "1:300,650,560,740" --reason Approved --password-fd 0 < "$OUT/pw"
    expect 0 "verify two signatures"         verify "$OUT/sig2.pdf" --trust "$OUT/ca.pem"
    [[ "$(grep -c 'intact' "$OUT/stdout")" == 2 ]] || { echo "FAIL  both signatures should be intact"; failures=$((failures + 1)); }

    # Editing a signed document after the fact: the signature survives, but the
    # change is reported, and the exit code says so.
    expect 0 "annotate a signed document"    annotate "$OUT/sig2.pdf" --note "1:60,60:later" -o "$OUT/sig3.pdf"
    expect 6 "verify: changed after signing" verify "$OUT/sig3.pdf" --trust "$OUT/ca.pem"
    grep -q "CHANGED" "$OUT/stdout" || { echo "FAIL  verify did not report the later change"; failures=$((failures + 1)); }

    # A flipped byte inside the signed range breaks it.
    cp "$OUT/sig.pdf" "$OUT/bad.pdf"
    printf 'X' | dd of="$OUT/bad.pdf" bs=1 seek=300 conv=notrunc status=none
    expect 4 "verify: broken signature"      verify "$OUT/bad.pdf" --trust "$OUT/ca.pem"

    # Redacting a signed file warns that it breaks the signatures.
    expect 0 "redact a signed document"      redact "$OUT/sig.pdf" --rect "1:72,100,300,120" -o "$OUT/sigredact.pdf"
    grep -q "signature" "$OUT/stderr" || { echo "FAIL  redact did not warn about signatures"; failures=$((failures + 1)); }
else
    echo "  skip  signing (openssl not installed)"
fi

# A picture stamp is not a signature, and the file says so.
expect 1 "--stamp-image without a box"     annotate "$IN" --stamp-image "1:$CORPUS/page.png" -o "$OUT/st.pdf"
expect 0 "--stamp-image"                   annotate "$IN" --stamp-image "1:$CORPUS/page.png:100,100,300,180" -o "$OUT/st.pdf"
expect 0 "the stamp is listed"             annots "$OUT/st.pdf"
grep -q "not a digital signature" "$OUT/stdout" || { echo "FAIL  the image stamp does not say what it is"; failures=$((failures + 1)); }

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
