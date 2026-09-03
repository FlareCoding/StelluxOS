#!/bin/sh
# Points packages.lock at a published release: downloads the release's
# SHA256SUMS and rewrites the release line and package entries, keeping
# the comments and the source line. The result is left uncommitted for
# review, since the pin states which package builds this tree works with.
#
# Usage: packages/pin.sh <release-tag>
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOCK="$SCRIPT_DIR/packages.lock"

TAG="${1:?usage: pin.sh <release-tag>}"
SOURCE="$(awk '$1 == "source" { print $2 }' "$LOCK")"
[ -n "$SOURCE" ] || { echo "pin: no source line in $LOCK" >&2; exit 1; }

sums="$(mktemp)"
trap 'rm -f "$sums" "$sums.lock"' EXIT
if ! curl -fsSL -o "$sums" "$SOURCE/$TAG/SHA256SUMS"; then
    echo "pin: no SHA256SUMS at $SOURCE/$TAG, is the release published?" >&2
    exit 1
fi

# Archive names are <name>-<version>-<release>-<arch>.tar.zst, read from
# the right so package names may themselves contain dashes
{
    awk '{ print } $1 == "source" { exit }' "$LOCK"
    echo
    echo "release $TAG"
    while read -r sum file; do
        stem="${file%.tar.zst}"
        arch="${stem##*-}"; stem="${stem%-*}"
        rel="${stem##*-}"; stem="${stem%-*}"
        ver="${stem##*-}"; name="${stem%-*}"
        echo "$name $ver-$rel $arch $sum"
    done < "$sums" | sort -k1,1 -k3,3
} > "$sums.lock"
mv "$sums.lock" "$LOCK"

echo "pin: $LOCK now points at $TAG, review and commit:"
git -C "$SCRIPT_DIR" --no-pager diff -- packages.lock
