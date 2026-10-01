# Stellux developer packages

Stellux programs too heavy to build on every clone, such as the GCC
toolchain and CPython, ship as packages: static binaries built for Stellux once in a
container, published on a GitHub release, and pulled into the image.

## The registry

`packages.conf` lists every package and its tier. Packages in the
`default` tier are part of every image unless left out, packages in the
`extra` tier are included only by name.

## Selecting packages

`PACKAGES` selects packages the same way everywhere: it starts from the
default tier, a name adds a package, `-name` removes one, and `none` or
`all` replaces the default tier as the starting point.

    make image                          the default tier
    make image PACKAGES="-python"       the default tier without python
    make image PACKAGES="none gcc"      gcc alone
    make image PACKAGES=none            no packages at all
    make packages-list PACKAGES="all"   what every package resolves to

A name the registry does not know fails the build at once. `make test`
builds its images with `PACKAGES=none` unless told otherwise.

Fetching gets any archive missing from `userland/toolchain/packages/`,
checks it against the pinned sha256, and unpacks it into the rootfs
overlay that the userland install step copies onto the initrd. The cache
survives `make clean`, so the download happens once per version.
Unpacking needs `zstd` on the host, which `make deps` installs.

## Package format

`<name>-<version>-<release>-<arch>.tar.zst`, for example
`gcc-14.3.0-1-x86_64.tar.zst`. The archive is rooted at `/` so
unpacking it into a directory yields the exact tree the target sees.
`version` is the upstream version, `release` counts rebuilds of the
same upstream version with a changed recipe. Archives are reproducible:
sorted entries, root ownership, fixed timestamps, zstd level 19.

## The lock file

`packages.lock` is the single statement of which package builds this
tree works with. Every package depends on kernel features, so the pins
travel with the kernel sources they were tested against.

    source  <base URL of the GitHub releases hosting the archives>
    release <tag of the release every entry below was published under>
    <name> <version>-<release> <arch> <sha256>

A fetch that does not match its pinned sha256 fails the build.

## Recipes

Each package is built by the recipe `packages.conf` names, a directory
under `packages/` with a `build.sh <arch> [package...]` that leaves the
archives in the cache and a `versions.sh` whose `package_version <name>`
prints the version-release the recipe builds today. One recipe may emit
several packages: `toolchain` builds the GCC toolchain and CPython in one
container run and can emit gcc, binutils and python, leaving in the cache
only the ones it was asked for.

## Building and publishing

    make packages-build [PACKAGES=...] [ARCHES=x86_64]
    make packages-publish RELEASE=<tag>

A build only makes what a release does not already hold: a selected
package whose version-release `packages.lock` pins for that architecture
is fetched from the pinned release instead, and the recipes of the rest
run once per architecture. So bump a package's release number in its
recipe's `versions.sh` whenever the recipe changes, otherwise the next
build reuses the published archive. Both architectures are built unless
`ARCHES` says otherwise.

`make packages-publish` uploads the archives from the cache to a new
GitHub release. The `packages` workflow does the same on GitHub runners
and is the normal way to publish. After a release,
`make packages-pin RELEASE=<tag>` rewrites `packages.lock` from the
release's checksums; review the diff, boot the result once, and commit it.
