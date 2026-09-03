# Developer packages

Prebuilt tools that are too heavy to build on every clone, such as the
native GCC toolchain, ship as packages: archives built once in a
container, published on a GitHub release, and pulled into the image on
request. A plain `make image` stays lean and needs none of them.

## Using packages

    make image PACKAGES="gcc binutils"

Fetches any archive missing from `userland/toolchain/packages/`, checks
it against the pinned sha256, and unpacks it into the rootfs overlay
that the userland install step copies onto the initrd. The cache
survives `make clean`, so the download happens once per version.

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

## Building and publishing

    make packages-build                  builds every package for both
                                         architectures in Docker
    make packages-publish RELEASE=<tag>  uploads the archives from the
                                         cache to a new GitHub release

The `packages` workflow does the same on GitHub runners and is the
normal way to publish. After a release, update `packages.lock` with the
new tag and checksums.
