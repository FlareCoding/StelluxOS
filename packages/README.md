# Stellux developer packages

Stellux programs too heavy to build on every clone, such as the GCC
toolchain, ship as packages: static binaries built for Stellux once in a
container, published on a GitHub release, and pulled into the image on
request. A plain `make image` stays lean and needs none of them.

## Using packages

    make image PACKAGES="gcc binutils"

Fetches any archive missing from `userland/toolchain/packages/`, checks
it against the pinned sha256, and unpacks it into the rootfs overlay
that the userland install step copies onto the initrd. The cache
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

## Building and publishing

    make packages-build [ARCHES=x86_64]  builds every package, for both
                                         architectures by default, in Docker
    make packages-publish RELEASE=<tag>  uploads the archives from the
                                         cache to a new GitHub release

The `packages` workflow does the same on GitHub runners and is the
normal way to publish. After a release, `make packages-pin RELEASE=<tag>`
rewrites `packages.lock` from the release's checksums; review the diff,
boot the result once, and commit it. Bump a package's release number in
`versions.sh` whenever its recipe changes, so a changed archive always
gets a new name.
