#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# A short mutation-fuzz pass over every target, from the checked-in corpus.
# Usage: tools/fuzz-smoke.sh <build-dir> [iterations] [seconds]   (defaults 2000, 360)
#
# Only meaningful in a sanitizer build against MuPDF 1.28.4 or newer: 1.28.2
# aborts within a few dozen iterations on an upstream bug (tests/crashes/).
# A fixed seed per target keeps a failure reproducible from the log.
set -eu

build=$1
iterations=${2:-2000}
# Per target: stop at this many seconds even if iterations remain, so CI's
# step length stays fixed as targets get slower. 0 means no limit.
seconds=${3:-360}
corpus=$(dirname "$0")/../tests/corpus

export LSAN_OPTIONS="suppressions=$(dirname "$0")/../tests/lsan.supp"
export ASAN_OPTIONS=fast_unwind_on_malloc=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

# Quiet on success (the driver prints a line per iteration); on failure, the
# tail of the log, which holds the sanitizer report and the failing input.
log=$(mktemp)
trap 'rm -f "$log"' EXIT
for target in open ops edit verify ipc; do
    if "$build/fuzz/leht_fuzz_$target" "$corpus" "$iterations" 1234 "$seconds" >"$log" 2>&1; then
        echo "leht_fuzz_$target: $(tail -n 1 "$log")"
    else
        echo "leht_fuzz_$target FAILED:" >&2
        tail -n 60 "$log" >&2
        exit 1
    fi
done
