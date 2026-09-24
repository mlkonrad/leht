#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Everything CI needs on Fedora 44: build dependencies, the viewer's, and the
# test-time tools listed in CONTRIBUTING.md. None of the test tools is a
# runtime dependency of Leht. Used by .github/workflows/ci.yml and
# tools/ci-local.sh, so the two cannot drift apart.
set -eu
# The Cisco openh264 repository is a third-party host that a media dependency
# drags in, and the first CI run timed out on it. Fedora's own noopenh264 stub
# satisfies the dependency; nothing here plays video.
dnf -y install --setopt=install_weak_deps=False --disablerepo=fedora-cisco-openh264 \
    gcc-c++ make cmake ninja-build curl binutils \
    mupdf-devel openssl-devel libseccomp-devel p11-kit-devel \
    tesseract-devel leptonica-devel \
    qt6-qtbase-devel qt6-qtsvg \
    mupdf qpdf ghostscript poppler-utils openssl softhsm \
    tesseract-langpack-eng tesseract-langpack-est \
    python3 python3-pillow python3-numpy diffutils \
    desktop-file-utils libasan libubsan
