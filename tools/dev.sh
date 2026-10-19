#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Run a command in ONE's build container (docker/Dockerfile), in the current directory of the working tree:
#
#   tools/dev.sh cmake --preset linux-debugger
#   tools/dev.sh cmake --build --preset linux-debugger -j
#   tools/dev.sh ctest --preset linux-debugger
#   tools/dev.sh bash tools/format.sh
#   tools/dev.sh                      # an interactive shell
#
# Every run is a new container that is deleted when the command ends (docker run --rm): nothing is kept inside it.
# Everything a command writes goes into the working tree, which is mounted at the same path as on the host (so the
# paths in build/*/compile_commands.json and in error messages are the host's), and is owned by the caller.
# The image is built the first time, and again whenever docker/Dockerfile changes (its tag is a hash of the file).
#
# Environment: ONE_DEV_IMAGE (image name, default one-dev), DOCKER (default docker; podman works too),
# ONE_DEV_DOCKERFILE (a file in docker/, default Dockerfile; release.sh uses Dockerfile.appimage),
# ONE_QUICKREF=0 (an image without Chrome, which only the quick reference needs), ONE_DEV_MOUNTS="/dir1 /dir2"
# (directories outside the tree to mount read-only at the same path).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
docker=${DOCKER:-docker}
quickref=${ONE_QUICKREF:-1}
dockerfile=${ONE_DEV_DOCKERFILE:-Dockerfile}
hash=$( (cat "$root/docker/$dockerfile"; echo "ONE_QUICKREF=$quickref") | sha256sum | cut -c1-12)
image="${ONE_DEV_IMAGE:-one-dev}:$hash"

if [ -z "$("$docker" images -q "$image" 2>/dev/null)" ]; then
    echo "dev.sh: building the image $image (once per change of docker/Dockerfile)" >&2
    if [ "$(basename "$docker")" = docker ] && ! "$docker" buildx version >/dev/null 2>&1; then
        # without buildx, docker build falls back to the deprecated legacy builder (and says so at length)
        echo "dev.sh: Docker's buildx component is not installed; for BuildKit builds install it:" >&2
        echo "dev.sh:   sudo apt install docker-buildx-plugin   (Docker's own packages: docker-ce)" >&2
        echo "dev.sh:   sudo apt install docker-buildx          (Ubuntu / Mint packages: docker.io)" >&2
    fi
    "$docker" build --build-arg "ONE_QUICKREF=$quickref" -t "$image" -f "$root/docker/$dockerfile" "$root/docker" >&2
fi

# Work in the current directory if it is inside the tree, else at its root
case "$PWD/" in
"$root"/*) workdir=$PWD ;;
*) workdir=$root ;;
esac

options=(--rm --init -v "$root:$root" -w "$workdir")
for dir in ${ONE_DEV_MOUNTS:-}; do
    options+=(-v "$dir:$dir:ro") # more directories, read-only, at the same path (the games tools/release.sh trains on)
done
if [ "$(basename "$docker")" = podman ]; then
    options+=(--userns=keep-id) # rootless Podman: the caller's id inside, so that the files written are theirs
else
    options+=(--user "$(id -u):$(id -g)")
fi
if [ -t 0 ] && [ -t 1 ]; then
    options+=(-it) # a terminal: interactive (the debugger's UI and a shell need one)
fi
if [ $# -eq 0 ]; then
    set -- bash
fi
exec "$docker" run "${options[@]}" "$image" "$@"
