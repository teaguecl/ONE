#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Try the Linux AppImage (tools/release.sh) on several distributions, each in a container:
#
#   tools/test_appimage.sh APPIMAGE [IMAGE...]      (default images below; DOCKER=podman works too)
#
# In each: the AppImage is unpacked and run (a container has no FUSE), `--version` runs, and so does nestest (the
# debugger AppImage, ONE-Debugger-*, runs a batch script; the ONE-* one the emulator for a while), and, where the
# distribution has Xvfb, the real window opens on X11 (SDL's X11 backend, the audio device and OpenGL are loaded at run
# time from the system: this checks that they are found). A distribution whose glibc is older than the AppImage's must fail with "version `GLIBC_...'
# not found": images in EXPECT_FAIL are checked for that.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
docker=${DOCKER:-docker}
app=$(realpath "${1:?usage: tools/test_appimage.sh APPIMAGE [IMAGE...]}")
shift
[ -f "$app" ] || { echo "test_appimage.sh: no such file: $app" >&2; exit 2; }
images=("$@")
[ ${#images[@]} -gt 0 ] || images=(ubuntu:22.04 ubuntu:24.04 debian:12 fedora:40 archlinux:latest opensuse/leap:15.6)
expect_fail=" ${EXPECT_FAIL:-debian:11 ubuntu:20.04} "

case "$(basename "$app")" in ONE-Debugger-*) kind=debugger ;; *) kind=one ;; esac
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# what runs inside each container
cat >"$work/inner.sh" <<'INNER'
set -u
cd /tmp
cp /app/app.AppImage ./one.AppImage && chmod +x ./one.AppImage
cp /app/nestest.nes .
export APPIMAGE_EXTRACT_AND_RUN=1
export XDG_CONFIG_HOME=/tmp/cfg XDG_STATE_HOME=/tmp/state
./one.AppImage --version || exit 11
if [ "$KIND" = debugger ]; then
    printf 'version\nquit\n' >s.txt
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./one.AppImage --batch -s s.txt nestest.nes >batch.out 2>&1 || { cat batch.out; exit 12; }
else
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./one.AppImage -k 2000000 nestest.nes >batch.out 2>&1 || { cat batch.out; exit 12; }
fi
if command -v xvfb-run >/dev/null; then
    if [ "$KIND" = debugger ]; then
        SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 ONE_DEBUGGER_CAPTURE=/tmp/out.bmp \
            timeout 60 xvfb-run -a -s "-screen 0 1280x1024x24" ./one.AppImage -H --no-init nestest.nes >gui.out 2>&1
        [ -s /tmp/out.bmp ] || { echo "no window capture"; tail -20 gui.out; exit 13; }
        echo "window: X11 capture of $(stat -c %s /tmp/out.bmp) bytes"
    else
        SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 timeout 60 xvfb-run -a -s "-screen 0 1280x1024x24" ./one.AppImage -k 3000000 nestest.nes >gui.out 2>&1 ||
            { echo "no window"; tail -20 gui.out; exit 13; }
        echo "window: opened on X11 and ran"
    fi
else
    echo "window: not tried (no Xvfb for this distribution)"
fi
INNER

# Xvfb and the graphics drivers a desktop has, per distribution
prep() {
    case "$1" in
    ubuntu* | debian*) echo "apt-get update -qq && apt-get install -y -qq --no-install-recommends xvfb xauth libgl1 libgl1-mesa-dri libegl1" ;;
    fedora*) echo "dnf install -y -q xorg-x11-server-Xvfb xauth mesa-libGL mesa-dri-drivers" ;;
    archlinux*) echo "pacman -Sy --noconfirm xorg-server-xvfb xorg-xauth mesa" ;;
    opensuse*) echo "zypper -q -n install xvfb-run xauth Mesa-libGL1 Mesa-dri" ;;
    *) echo true ;;
    esac
}

fail=0
for image in "${images[@]}"; do
    echo "== $image"
    prep "$image" >"$work/prep.sh"
    log=$("$docker" run --rm -e KIND="$kind" -v "$app:/app/app.AppImage:ro" -v "$root/tests/data/nestest.nes:/app/nestest.nes:ro" \
        -v "$work:/work:ro" "$image" sh -c "sh /work/prep.sh >/tmp/prep.log 2>&1 || echo 'prep failed (no window test)'; sh /work/inner.sh" 2>&1)
    rc=$?
    echo "$log" | tail -5
    if [[ " $expect_fail " == *" $image "* ]]; then
        if [ $rc != 0 ]; then echo "-> failed as expected (older glibc)"; else echo "-> UNEXPECTED PASS"; fail=1; fi
    elif [ $rc = 0 ]; then
        echo "-> ok"
    else
        echo "-> FAILED (rc=$rc)"; fail=1
    fi
done
exit $fail
