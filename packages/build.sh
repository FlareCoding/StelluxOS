#!/bin/sh
# Builds the packages whose version-release packages.lock does not pin yet and
# fetches the pinned archives of the rest, running each recipe once per architecture.
#
# Usage: packages/build.sh "<arch ...>" <package ...>
# Called by make packages-build with the packages PACKAGES selects.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REGISTRY="$SCRIPT_DIR/packages.conf"
LOCK="$SCRIPT_DIR/packages.lock"
CACHE="$SCRIPT_DIR/../userland/toolchain/packages"

arches="${1:?usage: build.sh \"<arch ...>\" <package ...>}"
shift
[ $# -gt 0 ] || { echo "packages: no packages selected" >&2; exit 1; }

source="$(awk '$1 == "source" { print $2 }' "$LOCK")"
release="$(awk '$1 == "release" { print $2 }' "$LOCK")"

recipe_of() {
    awk -v n="$1" '$1 == n { print $3 }' "$REGISTRY"
}

# The version-release the recipe would build for a package today
current_version() {
    (. "$SCRIPT_DIR/$(recipe_of "$1")/versions.sh" && package_version "$1")
}

# The version-release the lock pins for a package and architecture, if any
pinned_version() {
    awk -v n="$1" -v a="$2" '$1 == n && $3 == a { print $2 }' "$LOCK"
}

pinned_sha256() {
    awk -v n="$1" -v a="$2" '$1 == n && $3 == a { print $4 }' "$LOCK"
}

# Downloads the pinned archive unless the cache already holds it
fetch_pinned() {
    file="$1-$(pinned_version "$1" "$2")-$2.tar.zst"
    if [ -f "$CACHE/$file" ]; then
        return 0
    fi

    mkdir -p "$CACHE"
    echo "=== packages: fetching $file from $release ==="
    curl -fsSL --retry 3 -o "$CACHE/$file.tmp" "$source/$release/$file"
    echo "$(pinned_sha256 "$1" "$2")  $CACHE/$file.tmp" | shasum -a 256 -c - > /dev/null || {
        rm -f "$CACHE/$file.tmp"
        echo "packages: downloaded $file does not match packages.lock" >&2
        exit 1
    }
    mv "$CACHE/$file.tmp" "$CACHE/$file"
}

for package in "$@"; do
    [ -n "$(recipe_of "$package")" ] || { echo "packages: $package is not in $REGISTRY" >&2; exit 1; }
done

# The selected packages of one recipe that the lock does not pin at their current version
unpublished_for() {
    for package in "$@"; do
        if [ "$(recipe_of "$package")" = "$recipe" ] &&
           [ "$(current_version "$package")" != "$(pinned_version "$package" "$arch")" ]; then
            printf '%s ' "$package"
        fi
    done
}

summary=""
for arch in $arches; do
    recipes=""
    for package in "$@"; do
        current="$(current_version "$package")"
        if [ "$current" = "$(pinned_version "$package" "$arch")" ]; then
            fetch_pinned "$package" "$arch"
            summary="$summary
$package $arch: $current reused from $release"
            continue
        fi

        recipe="$(recipe_of "$package")"
        case " $recipes " in *" $recipe "*) ;; *) recipes="$recipes $recipe" ;; esac
        summary="$summary
$package $arch: $current built"
    done

    for recipe in $recipes; do
        # shellcheck disable=SC2046
        "$SCRIPT_DIR/$recipe/build.sh" "$arch" $(unpublished_for "$@")
        for package in $(unpublished_for "$@"); do
            [ -f "$CACHE/$package-$(current_version "$package")-$arch.tar.zst" ] ||
                { echo "packages: the $recipe recipe did not emit $package for $arch" >&2; exit 1; }
        done
    done
done

echo "=== packages: done ===$summary"
(cd "$CACHE" && shasum -a 256 *.tar.zst)
