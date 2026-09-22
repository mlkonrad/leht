#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build the pinned MuPDF that Leht is tested against, for use with
#     cmake -DLEHT_MUPDF_ROOT=<printed path> ...
#
# Why not the distribution's: Fedora 44 ships 1.28.2, which aborts on a
# five-byte malformed file (tests/crashes/README.md); Debian and Ubuntu ship
# older still. 1.28.4 is the first release without that defect.
#
# Usage: tools/build-mupdf.sh [dest-dir]     (default: ~/.cache/leht)
# Idempotent: a finished tree is reused. Prints the tree's path on stdout last.
set -euo pipefail

VERSION=1.28.4
SHA256=2d97e043a616f96b148657c9c3d81ad71c4bd2052c59a2a3315ad842599340f9
URL="https://mupdf.com/downloads/archive/mupdf-${VERSION}-source.tar.gz"

dest="${1:-${XDG_CACHE_HOME:-$HOME/.cache}/leht}"
tree="${dest}/mupdf-${VERSION}-source"
stamp="${tree}/.leht-built"

mkdir -p "$dest"
if [[ -f "$stamp" ]]; then
    echo "$tree"
    exit 0
fi

tarball="${dest}/mupdf-${VERSION}-source.tar.gz"
if [[ ! -f "$tarball" ]]; then
    echo "downloading MuPDF ${VERSION}" >&2
    curl -fsSL -o "${tarball}.part" "$URL"
    mv "${tarball}.part" "$tarball"
fi
echo "${SHA256}  ${tarball}" | sha256sum -c --quiet - >&2 || {
    echo "checksum mismatch for ${tarball}; delete it and retry" >&2
    exit 1
}

rm -rf "$tree"
tar -xzf "$tarball" -C "$dest"

# HAVE_CURL=no: Leht never lets MuPDF fetch anything. X11/GLUT: viewer apps
# we do not build. -fPIC so the static archives link into PIE executables.
echo "building MuPDF ${VERSION} (a few minutes)" >&2
make -C "$tree" -j"$(nproc)" build=release \
     HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no \
     XCFLAGS=-fPIC libs >&2

touch "$stamp"
echo "$tree"
