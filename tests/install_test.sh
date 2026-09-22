#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# install_tree: `cmake --install` into a scratch DESTDIR, then check the tree.
# Arguments come from tests/CMakeLists.txt.
set -eu

build=$1 prefix=$2 bindir=$3 libexecdir=$4 datadir=$5 app_id=$6 ui=$7

dest=$(mktemp -d)
trap 'rm -rf "$dest"' EXIT
DESTDIR=$dest cmake --install "$build" >/dev/null
root=$dest$prefix

fail() { echo "FAIL: $*" >&2; exit 1; }
need() { [ -e "$root/$1" ] || fail "not installed: $prefix/$1"; }

need "$bindir/leht"
need "$libexecdir/leht/leht-worker"
if [ "$ui" = 1 ]; then
    need "$bindir/leht-viewer"
    need "$datadir/applications/$app_id.desktop"
    need "$datadir/icons/hicolor/scalable/apps/$app_id.svg"
    if command -v desktop-file-validate >/dev/null; then
        desktop-file-validate "$root/$datadir/applications/$app_id.desktop" ||
            fail "desktop-file-validate rejected $app_id.desktop"
    fi
    grep -q '^MimeType=application/pdf;' "$root/$datadir/applications/$app_id.desktop" ||
        fail "the desktop entry does not claim application/pdf"
fi

# Nothing installed may load libraries from the build tree.
for exe in "$root/$bindir"/leht* "$root/$libexecdir/leht/leht-worker"; do
    if command -v readelf >/dev/null &&
       readelf -d "$exe" | grep -E 'R(UN)?PATH' | grep -qF "$build"; then
        fail "$exe has an RPATH into the build tree"
    fi
done

# The installed programs run from where they landed.
"$root/$bindir/leht" --version >/dev/null || fail "installed leht --version failed"
# The worker must start and install its sandbox: opening a file must kill it
# with SIGSYS (159). 77 is a sanitizer build, which runs unsandboxed.
# The subshell keeps the shell's own "Bad system call" notice out of the log.
rc=$( ("$root/$libexecdir/leht/leht-worker" --selftest-sandbox=open) >/dev/null 2>&1 && echo 0 || echo $?)
[ $rc -eq 159 ] || [ $rc -eq 77 ] || fail "installed worker selftest exited $rc, expected 159"

echo "install tree OK ($prefix)"
