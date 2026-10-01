#!/bin/sh
# The chromium recipe: builds content_shell natively in a persistent Docker
# container, since the source tree is 10 GB and a clean build takes hours, and
# leaves the archive in the package cache.
#
# Usage: packages/chromium/build.sh <x86_64|aarch64> [chromium]
#   STEPS="fetch prepare configure build install" selects steps, STEPS=shell
#   opens a shell in the container, DETACH=1 runs the steps in the background
#   and logs to /work/build.log there, NINJA_FLAGS is handed to ninja.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [chromium]}"
host="$(uname -m)"
[ "$host" = arm64 ] && host=aarch64
if [ "$arch" != "$host" ]; then
    echo "chromium: builds natively, run this on an $arch machine (this one is $host)" >&2
    exit 1
fi

if ! docker info > /dev/null 2>&1; then
    echo "chromium: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

if ! docker container inspect "$CONTAINER" > /dev/null 2>&1; then
    docker volume create "$VOLUME" > /dev/null
    docker run -d --name "$CONTAINER" \
        --cpus "$BUILD_JOBS" --memory "$BUILD_MEMORY" --memory-swap "$BUILD_MEMORY" \
        --ulimit nofile=65536:65536 \
        -v "$VOLUME:/work" -v "$REPO_ROOT:/stellux" \
        "$ALPINE_IMAGE" sleep infinity > /dev/null
    # shellcheck disable=SC2086
    docker exec "$CONTAINER" apk add --no-cache $ALPINE_PKGS
elif [ "$(docker container inspect -f '{{.State.Running}}' "$CONTAINER")" != "true" ]; then
    docker start "$CONTAINER" > /dev/null
fi

steps="${STEPS:-fetch prepare configure build install}"
if [ "$steps" = shell ]; then
    exec docker exec -it -w "/work/src/chromium-$CHROMIUM_VER" "$CONTAINER" bash
fi

if [ "${DETACH:-0}" = "1" ]; then
    docker exec -d -e NINJA_FLAGS="${NINJA_FLAGS:-}" "$CONTAINER" bash -c \
        "bash /stellux/packages/chromium/container.sh $steps > /work/build.log 2>&1; echo \"STEPS_EXIT: \$?\" >> /work/build.log"
    echo "chromium: running '$steps' in the background, follow with:"
    echo "  docker exec $CONTAINER tail -f /work/build.log"
    exit 0
fi

# shellcheck disable=SC2086
exec docker exec -e NINJA_FLAGS="${NINJA_FLAGS:-}" "$CONTAINER" \
    bash /stellux/packages/chromium/container.sh $steps
