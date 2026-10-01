#!/bin/sh
# Publishes a complete set of archives, every registry package for every architecture
# it is built for, as a GitHub release tagged at the current commit. Archives in the
# directory are used as they are and the rest come from the pinned release. The release
# carries SHA256SUMS, RECIPES with each recipe's tree, and notes with the lock lines.
#
# Usage: packages/publish.sh <release-tag> [archive-dir]
# The directory defaults to userland/toolchain/packages, DRY_RUN=1 only shows the set.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/common.sh"

TAG="${1:?usage: publish.sh <release-tag> [archive-dir]}"
DIR="${2:-$CACHE}"
COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD)"

mkdir -p "$DIR"
files=""
recipes=""
for package in $(registry_packages); do
    recipe="$(recipe_of "$package")"
    case " $recipes " in *" $recipe "*) ;; *) recipes="$recipes $recipe" ;; esac

    for arch in $(arches_of "$package"); do
        current="$(current_version "$package")"
        file="$package-$current-$arch.tar.zst"
        if [ "$current" = "$(pinned_version "$package" "$arch")" ]; then
            check_reuse "$package"
            fetch_pinned "$package" "$arch" "$DIR"
        elif [ ! -f "$DIR/$file" ]; then
            echo "publish: $file is missing, run make packages-build PACKAGES=\"none $package\" ARCHES=$arch" >&2
            exit 1
        fi

        files="$files $file"
    done
done

cd "$DIR"
# shellcheck disable=SC2086
shasum -a 256 $files > SHA256SUMS
: > RECIPES
for recipe in $recipes; do
    echo "$recipe $(recipe_hash "$recipe")" >> RECIPES
done

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
    while read -r recipe hash; do
        echo "recipe $recipe $hash"
    done < RECIPES
    echo '```'
} > "$notes"

if [ "${DRY_RUN:-0}" = "1" ]; then
    echo "publish: would create release $TAG at $COMMIT with:"
    cat SHA256SUMS RECIPES
    rm -f "$notes"
    exit 0
fi

# shellcheck disable=SC2086
gh release create "$TAG" --target "$COMMIT" --title "$TAG" \
    --notes-file "$notes" $files SHA256SUMS RECIPES
rm -f "$notes"
