# Stellux developer packages

Stellux programs too heavy to build on every clone, such as the GCC
toolchain and CPython, ship as packages: static binaries built for Stellux once in a
container, published on a GitHub release, and pulled into the image. Libraries
the tree's own programs compile against ship the same way, as build packages.

## The registry

`packages.conf` lists every package and its tier. Packages in the
`default` tier are part of every image unless left out, packages in the
`extra` tier are included only by name. Packages in the `build` tier are
never part of an image: every userland build unpacks them into
`userland/toolchain/sdk/<arch>/` for the tree's programs to compile and
link against.

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
builds its images with `PACKAGES=none` unless told otherwise. A build
package in the selection is built, published and listed like any other,
but never staged into the image.

Fetching gets any archive missing from `userland/toolchain/packages/`,
checks it against the pinned sha256, and unpacks it into the rootfs
overlay that the userland install step copies onto the initrd. The cache
survives `make clean`, so the download happens once per version.
Unpacking needs `zstd` on the host, which `make deps` installs.

Build packages are fetched the same way, whatever `PACKAGES` selects, and
unpacked into the SDK directory before any library or app is built. The
SDK survives `make clean` like the cache, and a build package whose pin
changes is unpacked again.

## Package format

`<name>-<version>-<release>-<arch>.tar.zst`, for example
`gcc-14.3.0-1-x86_64.tar.zst`. The archive is rooted at `/` so
unpacking it into a directory yields the exact tree the target sees. A
build package's archive is rooted at the SDK directory instead, holding
one directory named after the package.
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

## The chromium recipe

`chromium` is an `extra` package: the Chromium browser built for Stellux
with an Ozone platform for the display manager. It is built
natively, never cross: `make packages-build PACKAGES="none chromium"
ARCHES=<arch>` on a machine of that architecture with Docker, about 60 GB
of disk and the memory `BUILD_MEMORY` in `packages/chromium/versions.sh`
names. The source tree is 10 GB and a clean build takes hours, so the
recipe keeps its container and volume between runs. `STEPS` and `DETACH`
are described in `packages/chromium/build.sh`. The package brings its own
dock launcher through the desktop's drop-in directory. The browser wants
more memory than QEMU's 4 GB default, so run it with `QEMU_MEMORY=8G`. A
version bump means refreshing `patches/alpine` and `patches/copium` from
the aports commit that packages the same release.

## The qt recipe

`qt` is a `build` package: static QtCore, QtGui, QtWidgets and QtSvg
libraries cross-built with the tree's own compiler, C++ runtime and musl,
which the recipe builds in its container from the committed tree. The kit
carries `share/stellux/qt.mk`, the flags a program compiles and links with,
measured on the recipe's probe programs, and Qt's license texts under
`qt/LICENSES`. Qt is LGPLv3: the recipe pins the exact upstream sources, and
the programs linked against the kit are built from this tree, so anyone can
relink them against a modified Qt. A run takes about 10 minutes per
architecture on 16 cores, plus the sysroot and the host Qt on the first run,
both kept in the `stellux-qt-work` volume.

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

`make packages-publish RELEASE=<tag>` creates a GitHub release holding a
complete set: every registry package for every architecture it is built
for. Archives in the cache are uploaded as they are and the rest are
taken from the pinned release, so building one package is enough for a
release the lock can point at. `DRY_RUN=1` shows the set without
creating the release.

The `packages` workflow is the normal way to publish: dispatched by hand
with a release tag, a `PACKAGES` selection (empty for the default tier)
and the architectures, it plans one job per recipe and architecture on
the runner the registry names, builds or reuses, publishes the complete
release, and opens a pull request that pins it. A package built on a
developer machine can be published from there with
`make packages-publish` instead, and the next workflow run reuses it. For
the pull request step, the repository must allow Actions to create pull
requests.

## Working on a recipe

Bump the package's release number, build it, and `make image` uses the
new archive from the cache: a package whose recipe names a version the
lock does not pin, and whose archive for that version is in the cache, is
staged as it is, with a line saying it is an unpublished local build.
Nothing pinned changes, and the first build of a new package works the
same way. Once published and pinned, the checksum check applies again.

A release also records the committed tree of each recipe it was built
from, which `make packages-pin RELEASE=<tag>` copies into `packages.lock`
along with the checksums. A build then refuses to reuse a published
archive whose recipe changed since, so a recipe change without a release
bump is caught instead of shipping a stale archive. After pinning, review
the diff, boot the result once, and commit it.
