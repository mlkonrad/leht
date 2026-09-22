#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Run one CI job locally, in the same Fedora 44 container GitHub uses:
#     tools/ci-local.sh system|pinned|asan
#
# The source tree is mounted read-only and built in the container, so a
# missing dependency or a test that leans on this machine fails here first.
# The MuPDF build is kept in a podman volume between runs.
set -eu

job=${1:?usage: $0 system|pinned|asan}
case $job in
    system) args="-DCMAKE_BUILD_TYPE=RelWithDebInfo" ;;
    pinned) args="-DCMAKE_BUILD_TYPE=RelWithDebInfo" ;;
    asan)   args="-DCMAKE_BUILD_TYPE=Debug -DLEHT_SANITIZE=ON" ;;
    *) echo "unknown job: $job" >&2; exit 2 ;;
esac

src=$(cd "$(dirname "$0")/.." && pwd)
exec podman run --rm --init \
    -v "$src:/src:ro,z" \
    -v leht-ci-cache:/opt/leht-cache \
    -e JOB="$job" -e ARGS="$args" \
    registry.fedoraproject.org/fedora:44 \
    sh -euc '
        /src/tools/ci-deps.sh >/dev/null
        root=""
        if [ "$JOB" != system ]; then
            root="-DLEHT_MUPDF_ROOT=$(/src/tools/build-mupdf.sh /opt/leht-cache)"
        fi
        cmake -S /src -B /build -G Ninja -DLEHT_BUILD_UI=ON $ARGS $root
        cmake --build /build
        ctest --test-dir /build --output-on-failure -j "$(nproc)"
        if [ "$JOB" = asan ]; then /src/tools/fuzz-smoke.sh /build; fi
    '
