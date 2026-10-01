#!/bin/sh
# Builds the packages whose version-release packages.lock does not pin yet and
# fetches the pinned archives of the rest, running each recipe once per architecture.
#
# Usage: packages/build.sh "<arch ...>" <package ...>
# Called by make packages-build with the packages PACKAGES selects.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/common.sh"

arches="${1:?usage: build.sh \"<arch ...>\" <package ...>}"
shift
[ $# -gt 0 ] || { echo "packages: no packages selected" >&2; exit 1; }

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
            check_reuse "$package"
            fetch_pinned "$package" "$arch" "$CACHE"
            summary="$summary
$package $arch: $current reused from $(lock_release)"
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
