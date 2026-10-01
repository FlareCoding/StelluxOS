#!/bin/sh
# Prints the publish workflow's build matrix as JSON: one job per recipe and
# architecture over the selected packages, each carrying the packages it builds
# and the runner label the registry names for them.
#
# Usage: packages/plan.sh "<arch ...>" <package ...>
# Called by make packages-plan with the packages PACKAGES selects.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/common.sh"

arches="${1:?usage: plan.sh \"<arch ...>\" <package ...>}"
shift
[ $# -gt 0 ] || { echo "packages: no packages selected" >&2; exit 1; }

runner_of() {
    awk -v n="$1" '$1 == n { print $5 }' "$REGISTRY"
}

# The selected packages of one recipe that are built for an architecture
packages_for() {
    for package in "$@"; do
        case " $(arches_of "$package") " in *" $arch "*) ;; *) continue ;; esac
        if [ "$(recipe_of "$package")" = "$recipe" ]; then
            printf '%s ' "$package"
        fi
    done
}

jobs=""
for arch in $arches; do
    recipes=""
    for package in "$@"; do
        recipe="$(recipe_of "$package")"
        [ -n "$recipe" ] || { echo "packages: $package is not in $REGISTRY" >&2; exit 1; }
        case " $recipes " in *" $recipe "*) ;; *) recipes="$recipes $recipe" ;; esac
    done

    for recipe in $recipes; do
        packages="$(packages_for "$@")"
        [ -n "$packages" ] || continue

        runner=""
        for package in $packages; do
            labels="$(runner_of "$package" | sed "s/{arch}/$arch/g")"
            if [ -n "$runner" ] && [ "$runner" != "$labels" ]; then
                echo "packages: the packages of the $recipe recipe name different runners" >&2
                exit 1
            fi
            runner="$labels"
        done

        # runs-on takes the labels as a list, so every one of them must match
        runner="[\"$(echo "$runner" | sed 's/,/","/g')\"]"
        job="{\"recipe\":\"$recipe\",\"arch\":\"$arch\",\"packages\":\"${packages% }\",\"runner\":$runner}"
        jobs="$jobs${jobs:+,}$job"
    done
done

[ -n "$jobs" ] || { echo "packages: none of the selected packages is built for: $arches" >&2; exit 1; }
echo "{\"include\":[$jobs]}"
