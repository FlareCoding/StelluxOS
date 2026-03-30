#!/bin/bash
#
# Regenerate trust_anchors.c from the Mozilla CA certificate bundle.
#
# Run this script manually whenever the CA bundle needs updating.
# The output (trust_anchors.c) is checked into the repository so
# that normal builds do not require brssl or network access.
#
# Prerequisites:
#   - BearSSL must be built natively first (cd bearssl && make)
#   - curl must be available for downloading the CA bundle
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRSSL="$SCRIPT_DIR/bearssl/build/brssl"
CACERT="$SCRIPT_DIR/cacert.pem"
OUTPUT="$SCRIPT_DIR/trust_anchors.c"
CACERT_URL="https://curl.se/ca/cacert.pem"

if [ ! -x "$BRSSL" ]; then
    echo "Building brssl host tool..."
    make -C "$SCRIPT_DIR/bearssl" -j"$(nproc)"
fi

if [ ! -f "$CACERT" ]; then
    echo "Downloading Mozilla CA bundle..."
    curl -sL -o "$CACERT" "$CACERT_URL"
fi

echo "Generating trust anchors..."
GENERATED=$("$BRSSL" ta "$CACERT" 2>/dev/null)

COUNT=$(echo "$GENERATED" | grep -o '#define TAs_NUM.*' | grep -oE '[0-9]+')

{
    echo '#include <bearssl.h>'
    echo '#include <stddef.h>'
    echo ''
    echo "$GENERATED" | sed 's/^static const br_x509_trust_anchor TAs\[/const br_x509_trust_anchor TAs[/'
} | sed "s/#define TAs_NUM   ${COUNT}/const size_t TAs_NUM = ${COUNT};/" > "$OUTPUT"

echo "Generated $OUTPUT with $COUNT trust anchors."
