#!/bin/bash
# Rebuild and deploy the extension into Cursor's extensions directory.
# After running this, do: Ctrl+Shift+P -> "Developer: Reload Window"

set -e
cd "$(dirname "$0")"

echo "Building..."
node esbuild.js

DEST="$HOME/.cursor/extensions/stellux.stellux-dynpriv-0.1.0"
echo "Deploying to $DEST..."
mkdir -p "$DEST/dist"
cp package.json "$DEST/"
cp dist/extension.js dist/extension.js.map "$DEST/dist/"

echo "Done. Reload Cursor to pick up changes."
