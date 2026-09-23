#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Run one CI job locally, in the same Fedora 44 container GitHub uses:
#     tools/ci-local.sh system|pinned|asan
#
# The container gets what a fresh checkout would: the files git knows about
# (tracked, plus new files not ignored), uncommitted edits included -- but
# nothing ignored, so a generated corpus or a stray build here cannot hide a
# step CI is missing. The MuPDF build is kept in a podman volume between runs.
set -eu

job=${1:?usage: $0 system|pinned|asan}
case $job in
    system) args="-DCMAKE_BUILD_TYPE=RelWithDebInfo" ;;
    pinned) args="-DCMAKE_BUILD_TYPE=RelWithDebInfo" ;;
    asan)   args="-DCMAKE_BUILD_TYPE=Debug -DLEHT_SANITIZE=ON" ;;
    *) echo "unknown job: $job" >&2; exit 2 ;;
esac

cd "$(dirname "$0")/.."
git ls-files -z --cached --others --exclude-standard |
    xargs -0 sh -c 'for f; do [ -e "$f" ] && printf "%s\0" "$f"; done' _ |
    tar --null -T - -cf - |
exec podman run --rm --init -i \
    -v leht-ci-cache:/opt/leht-cache \
    -e JOB="$job" -e ARGS="$args" \
    registry.fedoraproject.org/fedora:44 \
    sh -euc '
        mkdir /src && tar -xf - -C /src
        /src/tools/ci-deps.sh >/dev/null
        root=""
        if [ "$JOB" != system ]; then
            root="-DLEHT_MUPDF_ROOT=$(/src/tools/build-mupdf.sh /opt/leht-cache)"
        fi
        cmake -S /src -B /build -G Ninja -DLEHT_BUILD_UI=ON $ARGS $root
        cmake --build /build
        LSAN_OPTIONS=suppressions=/src/tests/lsan.supp ASAN_OPTIONS=fast_unwind_on_malloc=0 \
            LEHT_BIN=/build/cli/leht /src/tests/corpus/generate.sh
        ctest --test-dir /build --output-on-failure -j "$(nproc)" --timeout 600
        if [ "$JOB" = asan ]; then /src/tools/fuzz-smoke.sh /build; fi
    '
