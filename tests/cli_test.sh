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
