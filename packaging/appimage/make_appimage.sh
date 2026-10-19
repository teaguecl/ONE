#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Make an AppImage from built programs. Runs in the AppImage build container (docker/Dockerfile.appimage), from the
# root of the tree (tools/release.sh does that):
#
#   packaging/appimage/make_appimage.sh APP TITLE VERSION OUTDIR PROGRAM...
#
# APP is one or one-debugger (it picks the .desktop file and icon in this directory, and the program AppRun starts);
# TITLE the file name's first part ("ONE", "ONE-Debugger"). The result: OUTDIR/TITLE-VERSION-x86_64.AppImage.
set -euo pipefail
app=$1 title=$2 version=$3 out=$4
shift 4
here=packaging/appimage
appdir=$out/AppDir.$app
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/doc/one"
cp "$@" "$appdir/usr/bin/"
strip "$appdir"/usr/bin/*
echo "$app" >"$appdir/app"
cp "$here/AppRun" "$here/$app.desktop" "$here/$app.png" "$appdir/"
cp LICENSE README.md THIRD_PARTY.md docs/usage.md docs/lua.md "$appdir/usr/share/doc/one/"
mkdir "$appdir/usr/share/doc/one/licenses" # (the MIT, zlib and ISC licenses want their notices with every binary)
cp third_party/licenses/*.txt "$appdir/usr/share/doc/one/licenses/"
cp third_party/libco/LICENSE "$appdir/usr/share/doc/one/licenses/libco-ISC.txt"
if [ "$app" = one-debugger ]; then
    cp docs/debugger.md docs/quickref/one-debugger-quickref.pdf "$appdir/usr/share/doc/one/"
fi
ARCH=x86_64 appimagetool --runtime-file /opt/runtime-x86_64 "$appdir" "$out/$title-$version-x86_64.AppImage"
