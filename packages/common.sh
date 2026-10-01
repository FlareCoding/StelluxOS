# Shared by the package scripts: the registry, the lock, the recipes, and reusing
# published archives. Sourced with SCRIPT_DIR set to this directory.

REGISTRY="$SCRIPT_DIR/packages.conf"
LOCK="$SCRIPT_DIR/packages.lock"
CACHE="$SCRIPT_DIR/../userland/toolchain/packages"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

lock_source() {
    awk '$1 == "source" { print $2 }' "$LOCK"
}

lock_release() {
    awk '$1 == "release" { print $2 }' "$LOCK"
}

registry_packages() {
    awk '$1 !~ /^#/ && NF { print $1 }' "$REGISTRY"
}

recipe_of() {
    awk -v n="$1" '$1 == n { print $3 }' "$REGISTRY"
}

# The architectures a package is built for, space separated
arches_of() {
    awk -v n="$1" '$1 == n { print $4 }' "$REGISTRY" | tr ',' ' '
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

# The committed tree of a recipe, which identifies what a release was built from
recipe_hash() {
    git -C "$REPO_ROOT" rev-parse "HEAD:packages/$1"
}

pinned_recipe_hash() {
    awk -v r="$1" '$1 == "recipe" && $2 == r { print $3 }' "$LOCK"
}

# A published archive may stand in for a build only while its recipe is the one
# it was built from, otherwise the recipe changed without a release bump
check_reuse() {
    recipe="$(recipe_of "$1")"
    pinned="$(pinned_recipe_hash "$recipe")"
    if [ -n "$(git -C "$REPO_ROOT" status --porcelain -- "packages/$recipe")" ]; then
        echo "packages: packages/$recipe has uncommitted changes, commit or revert them before reusing $1" >&2
        exit 1
    fi

    if [ -n "$pinned" ] && [ "$pinned" != "$(recipe_hash "$recipe")" ]; then
        echo "packages: packages/$recipe changed since $(lock_release) without a release bump for $1" >&2
        exit 1
    fi
}

# Downloads the pinned archive of a package into a directory, or checks the copy there
fetch_pinned() {
    file="$1-$(pinned_version "$1" "$2")-$2.tar.zst"
    if [ -f "$3/$file" ]; then
        echo "$(pinned_sha256 "$1" "$2")  $3/$file" | shasum -a 256 -c - > /dev/null || {
            echo "packages: $3/$file differs from the pinned archive, delete it to refetch" >&2
            exit 1
        }
        return 0
    fi

    mkdir -p "$3"
    echo "=== packages: fetching $file from $(lock_release) ==="
    curl -fsSL --retry 3 -o "$3/$file.tmp" "$(lock_source)/$(lock_release)/$file"
    echo "$(pinned_sha256 "$1" "$2")  $3/$file.tmp" | shasum -a 256 -c - > /dev/null || {
        rm -f "$3/$file.tmp"
        echo "packages: downloaded $file does not match packages.lock" >&2
        exit 1
    }
    mv "$3/$file.tmp" "$3/$file"
}
