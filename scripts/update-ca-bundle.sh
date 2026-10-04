#!/bin/sh
# Installs the pinned snapshot of Mozilla's CA certificates as the image's bundle and generates
# BearSSL's compiled-in trust anchors from it. Run by hand after changing the pin, commit both.
#
# Usage: scripts/update-ca-bundle.sh
set -eu

# curl's extract of Mozilla's CA certificates, https://curl.se/docs/caextract.html
CA_BUNDLE_DATE=2026-09-25
CA_BUNDLE_SHA256=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bearssl="$REPO_ROOT/userland/lib/libbearssl"
brssl="$bearssl/bearssl/build/brssl"

# Debian's bundle path, the first place TLS programs built for Linux look
bundle="$REPO_ROOT/initrd/etc/ssl/certs/ca-certificates.crt"

if ! echo "$CA_BUNDLE_SHA256  $bundle" | shasum -a 256 -c - > /dev/null 2>&1; then
    echo "Downloading the CA bundle of $CA_BUNDLE_DATE..."
    mkdir -p "$(dirname "$bundle")"
    curl -fsSL --retry 3 -o "$bundle.tmp" "https://curl.se/ca/cacert-$CA_BUNDLE_DATE.pem"
    echo "$CA_BUNDLE_SHA256  $bundle.tmp" | shasum -a 256 -c - > /dev/null || {
        rm -f "$bundle.tmp"
        echo "update-ca-bundle: the CA bundle of $CA_BUNDLE_DATE does not match its pinned checksum" >&2
        exit 1
    }
    mv "$bundle.tmp" "$bundle"
fi

if [ ! -x "$brssl" ]; then
    echo "Building the brssl host tool..."
    make -C "$bearssl/bearssl" -j"$(getconf _NPROCESSORS_ONLN)" tools > /dev/null
fi

anchors="$("$brssl" ta -q "$bundle")"
count="$(echo "$anchors" | sed -n 's/^#define TAs_NUM *\([0-9]*\)$/\1/p')"
{
    echo "/* Generated from initrd/etc/ssl/certs/ca-certificates.crt by scripts/update-ca-bundle.sh */"
    echo '#include <bearssl.h>'
    echo '#include <stddef.h>'
    echo ''
    echo "$anchors" | sed -e 's/^static const br_x509_trust_anchor TAs\[/const br_x509_trust_anchor TAs[/' \
        -e "s/^#define TAs_NUM .*/const size_t TAs_NUM = $count;/"
} > "$bearssl/trust_anchors.c"

echo "Installed the CA bundle of $CA_BUNDLE_DATE and generated $count BearSSL trust anchors"
