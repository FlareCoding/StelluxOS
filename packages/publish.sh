#!/bin/sh
# Publishes Stellux package archives as a GitHub release tagged at the
# current commit, with a SHA256SUMS file and release notes holding the
# lines to paste into packages.lock.
#
# Usage: packages/publish.sh <release-tag> [archive-dir]
# The archive directory defaults to userland/toolchain/packages.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TAG="${1:?usage: publish.sh <release-tag> [archive-dir]}"
DIR="${2:-$REPO_ROOT/userland/toolchain/packages}"
COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD)"

cd "$DIR"
if ! ls ./*.tar.zst > /dev/null 2>&1; then
    echo "publish: no package archives in $DIR" >&2
    exit 1
fi

shasum -a 256 ./*.tar.zst | sed 's|\./||' > SHA256SUMS

# Archive names are <name>-<version>-<release>-<arch>.tar.zst, read from
# the right so package names may themselves contain dashes
notes="$(mktemp)"
{
    echo "Developer packages built from $COMMIT."
    echo
    echo "packages.lock entries:"
    echo
    echo '```'
    echo "release $TAG"
    while read -r sum file; do
        stem="${file%.tar.zst}"
        arch="${stem##*-}"; stem="${stem%-*}"
        rel="${stem##*-}"; stem="${stem%-*}"
        ver="${stem##*-}"; name="${stem%-*}"
        echo "$name $ver-$rel $arch $sum"
    done < SHA256SUMS
    echo '```'
} > "$notes"

gh release create "$TAG" --target "$COMMIT" --title "$TAG" \
    --notes-file "$notes" ./*.tar.zst SHA256SUMS
rm -f "$notes"

echo "publish: $TAG released, lock entries are in the release notes"
