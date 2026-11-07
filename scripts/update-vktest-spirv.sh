#!/bin/sh
# Assembles each of vktest's SPIR-V assembly sources with SPIRV-Tools into the header of words vktest compiles in,
# validated for Vulkan 1.4, the version vktest's instance asks for. Run by hand after changing a .spvasm, commit both.
set -eu

TARGET_ENV=vulkan1.4

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
src="$REPO_ROOT/userland/apps/vktest/src"
module="$(mktemp)"
trap 'rm -f "$module"' EXIT

for source in "$src"/*.spvasm; do
    name="$(basename "$source" .spvasm)"
    upper="$(echo "$name" | tr '[:lower:]' '[:upper:]')"

    spirv-as --target-env "$TARGET_ENV" -o "$module" "$source"
    spirv-val --target-env "$TARGET_ENV" "$module"
    words="$(od -An -v -tx4 "$module")"

    {
        echo "// Generated from $name.spvasm by scripts/update-vktest-spirv.sh"
        echo ''
        echo "#ifndef ${upper}_SPIRV_H"
        echo "#define ${upper}_SPIRV_H"
        echo ''
        echo '#include <stdint.h>'
        echo ''
        echo "constexpr uint32_t ${upper}_SPIRV[] = {"
        printf '%s\n' "$words" | awk '
            { for (i = 1; i <= NF; i++) words[count++] = $i }
            END {
                for (i = 0; i < count; i++) {
                    printf "%s0x%s,", (i % 8 == 0 ? "    " : " "), words[i]
                    if (i % 8 == 7 || i == count - 1) printf "\n"
                }
            }'
        echo '};'
        echo ''
        echo "#endif // ${upper}_SPIRV_H"
    } > "$src/${name}_spirv.h"

    echo "Generated ${name}_spirv.h from $name.spvasm"
done
