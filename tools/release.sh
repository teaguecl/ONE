#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Build everything a release contains, from the checked-out source, in the build container (tools/dev.sh):
#
#   tools/release.sh VERSION [--dist DIR] [--only a,b,...] [--quick] [--upload]
#
#   one-libretro-windows-x64-pgo        one_libretro.dll  MinGW-w64 cross build, profile-guided, trained under Wine
#   one-libretro-linux-x86_64-pgo       one_libretro.so   profile-guided, built on Ubuntu 22.04 (glibc 2.35)
#   one-windows-x64-pgo                 one.exe           MinGW-w64 cross build, profile-guided, trained under Wine
#   one-linux-x86_64-pgo-appimage       ONE-VERSION-x86_64.AppImage: one, profile-guided, SDL2 inside
#   one-debugger-windows-x64            one-debugger.exe  MinGW-w64 cross build (not profile-guided)
#   one-debugger-linux-x86_64-appimage  ONE-Debugger-VERSION-x86_64.AppImage: one-debugger, SDL2 inside
#   source                              ONE-VERSION-source.zip, from the tag vVERSION (HEAD if there is no such tag)
#
# The profile-guided builds are trained on the games listed in tools/pgo_games.txt, which are not part of the
# repository: put them in commercial_games/ (or the directory in ONE_GAMES_DIR; it is mounted into the container).
# The script stops before it builds anything if one of them is missing.
# The Windows debugger is tested under Wine (ctest --preset windows-cross-debugger); its remote channel is TCP only.
#
# Results: DIR (default build/dist/) holds the archives and SHA256SUMS.txt. --only builds just those (names above, without
# the version). --quick trains on nestest for 2 seconds and needs no games: to try the script (the result is not
# for a release). --upload makes a draft release vVERSION on GitHub from DIR with gh and RELEASE_NOTES.md.
# The Linux programs are built in Ubuntu 22.04 (docker/Dockerfile.appimage; glibc 2.35), the AppImages with SDL2 and
# libstdc++ linked in: they need nothing but glibc 2.35 or newer (Debian 12, Ubuntu 22.04, Mint 21, Fedora 36 and later).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

version=${1:-}
[ -n "$version" ] && [ "${version#-}" = "$version" ] || { sed -n 6,8p "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
shift
dist=$root/build/dist
only=""
quick=0
upload=0
while [ $# -gt 0 ]; do
    case "$1" in
    --dist) dist=$(realpath -m "$2"); shift ;;
    --only) only=$2; shift ;;
    --quick) quick=1 ;;
    --upload) upload=1 ;;
    *) echo "release.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
version=${version#v}
games=${ONE_GAMES_DIR:-$root/commercial_games}
all=(one-libretro-windows-x64-pgo one-libretro-linux-x86_64-pgo one-windows-x64-pgo one-linux-x86_64-pgo-appimage
    one-debugger-windows-x64 one-debugger-linux-x86_64-appimage source)
wanted() { [ -z "$only" ] || [[ ",$only," == *",$1,"* ]]; }

# the container: the games are mounted into it; ONE_QUICKREF=0, the image without Chrome (the PDF is committed)
export ONE_QUICKREF=0
case "$games/" in
"$root"/*) ;; # (in the tree: mounted with it)
*) ONE_DEV_MOUNTS="${ONE_DEV_MOUNTS:-} $games" ;;
esac
export ONE_DEV_MOUNTS
dev() { bash "$root/tools/dev.sh" "$@"; }
# the old-glibc container (docker/Dockerfile.appimage): the Linux programs are built there
old() { ONE_DEV_DOCKERFILE=Dockerfile.appimage ONE_DEV_IMAGE=one-appimage dev "$@"; }
# SDL2 and libstdc++ go into the program, so that it runs on any distribution with a glibc this new
static=(-DONE_FETCH_SDL2=ON "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc")

# 0. everything the builds need, before the hours begin
if [ $quick = 0 ]; then
    missing=0
    needs_games=0
    for name in "${all[@]}"; do
        wanted "$name" && [[ "$name" == *pgo* ]] && needs_games=1
    done
    if [ $needs_games = 1 ]; then
        while IFS= read -r name; do
            case "$name" in '' | '#'*) continue ;; esac
            [ -f "$games/$name" ] || { echo "release.sh: missing game: $games/$name" >&2; missing=1; }
        done <tools/pgo_games.txt
        [ $missing = 0 ] || { echo "release.sh: the games listed in tools/pgo_games.txt must be in $games" >&2; exit 1; }
    fi
fi
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    echo "release.sh: warning: uncommitted changes in the tree: the release is not exactly the commit" >&2
fi
mkdir -p "$dist"

pgo_env=(ONE_GAMES_DIR="$games")
if [ $quick = 1 ]; then
    pgo_env=(PGO_ROMS="$root/tests/data/nestest.nes" PGO_SECONDS=2)
else
    pgo_env+=(PGO_REQUIRE="$root/tools/pgo_games.txt")
fi

# stage NAME FILE...: dist/NAME-VERSION.{tar.gz,zip} with the programs and the documents next to them
stage() {
    local name=$1 kind=$2
    shift 2
    local top="$name-$version" tmp
    tmp=$(mktemp -d)
    mkdir -p "$tmp/$top/docs"
    cp "$@" "$tmp/$top/"
    cp LICENSE README.md THIRD_PARTY.md "$tmp/$top/"
    mkdir "$tmp/$top/licenses" # (the MIT, zlib and ISC licenses want their notices with every binary)
    cp third_party/licenses/*.txt "$tmp/$top/licenses/"
    cp third_party/libco/LICENSE "$tmp/$top/licenses/libco-ISC.txt"
    case "$name" in
    one-libretro-*) rmdir "$tmp/$top/docs" ;; # (RetroArch has the menu and the scripts: no documents of those)
    *)
        cp docs/usage.md docs/lua.md "$tmp/$top/docs/"
        mkdir -p "$tmp/$top/docs/examples" && cp lua/examples/*.lua lua/examples/README.md "$tmp/$top/docs/examples/"
        ;;
    esac
    case "$name" in
    *debugger*) cp docs/debugger.md "$tmp/$top/docs/" ;;
    esac
    rm -f "$dist/$top.tar.gz" "$dist/$top.zip"
    if [ "$kind" = zip ]; then
        (cd "$tmp" && python3 -m zipfile -c "$dist/$top.zip" "$top")
    else
        tar -C "$tmp" -czf "$dist/$top.tar.gz" "$top"
    fi
    rm -rf "$tmp"
    echo "release.sh: $top.$([ "$kind" = zip ] && echo zip || echo tar.gz)"
}

if wanted one-libretro-windows-x64-pgo; then
    echo "release.sh: == libretro, Windows, profile-guided (trained under Wine)"
    dev env "${pgo_env[@]}" bash tools/pgo.sh --libretro --windows build/release/libretro-windows
    stage one-libretro-windows-x64-pgo zip build/release/libretro-windows/one_libretro.dll libretro/one_libretro.info
fi
if wanted one-libretro-linux-x86_64-pgo; then
    echo "release.sh: == libretro, Linux, profile-guided (Ubuntu 22.04)"
    old env "${pgo_env[@]}" bash tools/pgo.sh --libretro build/release/libretro-linux
    stage one-libretro-linux-x86_64-pgo tar build/release/libretro-linux/one_libretro.so libretro/one_libretro.info
fi
if wanted one-windows-x64-pgo; then
    echo "release.sh: == one.exe, Windows, profile-guided (trained under Wine)"
    dev env "${pgo_env[@]}" bash tools/pgo.sh --windows build/release/one-windows -DONE_FETCH_SDL2=ON
    stage one-windows-x64-pgo zip build/release/one-windows/one.exe
fi
if wanted one-linux-x86_64-pgo-appimage; then
    echo "release.sh: == one, Linux, profile-guided, AppImage (Ubuntu 22.04, SDL2 built in)"
    old env "${pgo_env[@]}" bash tools/pgo.sh build/release/one-linux "${static[@]}"
    old bash packaging/appimage/make_appimage.sh one ONE "$version" build/release build/release/one-linux/one
    cp "build/release/ONE-$version-x86_64.AppImage" "$dist/"
    echo "release.sh: ONE-$version-x86_64.AppImage"
fi
if wanted one-debugger-windows-x64; then
    echo "release.sh: == one-debugger.exe, Windows (MinGW-w64 cross build)"
    dev bash -c "cmake --preset windows-cross-debugger -B build/release/debugger-windows \
            >/dev/null && cmake --build build/release/debugger-windows --target one-debugger --parallel"
    stage one-debugger-windows-x64 zip build/release/debugger-windows/debugger/one-debugger.exe \
        docs/quickref/one-debugger-quickref.pdf
fi
if wanted one-debugger-linux-x86_64-appimage; then
    echo "release.sh: == one-debugger, Linux, AppImage (Ubuntu 22.04, SDL2 built in)"
    old bash -c "cmake -S . -B build/release/debugger -DCMAKE_BUILD_TYPE=Release -DONE_BUILD_DEBUGGER=ON '${static[0]}' \
            '${static[1]}' >/dev/null && cmake --build build/release/debugger --target one-debugger --parallel"
    old bash packaging/appimage/make_appimage.sh one-debugger ONE-Debugger "$version" build/release \
        build/release/debugger/debugger/one-debugger
    cp "build/release/ONE-Debugger-$version-x86_64.AppImage" "$dist/"
    echo "release.sh: ONE-Debugger-$version-x86_64.AppImage"
fi
if wanted source; then
    ref=HEAD
    git rev-parse -q --verify "refs/tags/v$version" >/dev/null && ref=v$version
    git archive --format=zip --prefix="ONE-$version/" -o "$dist/ONE-$version-source.zip" "$ref"
    echo "release.sh: ONE-$version-source.zip (from $ref)"
fi

(cd "$dist" && shopt -s nullglob && sha256sum -- *.tar.gz *.zip *.AppImage >SHA256SUMS.txt)
echo "release.sh: done: $dist"
cat "$dist/SHA256SUMS.txt"

if [ $upload = 1 ]; then
    [ $quick = 0 ] || { echo "release.sh: --quick builds are not uploaded" >&2; exit 1; }
    gh release create "v$version" --draft --title "ONE $version" --notes-file RELEASE_NOTES.md \
        "$dist"/*.tar.gz "$dist"/*.zip "$dist"/*.AppImage "$dist/SHA256SUMS.txt"
fi
