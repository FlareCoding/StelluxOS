#!/usr/bin/env python3
"""Writes out GA102's bootloader and booters with NVIDIA's extraction tool, loaded unmodified,
as bootloader.bin, booter_load.bin and booter_unload.bin in an output directory.

The tool's main() converts every GPU family it knows and reads files the recipe does not
fetch, so this makes only the three calls main() makes for GA102, after setting the two
module variables main() would set.

Usage: extract.py <open kernel modules dir> <tool path in it> <firmware version> <output dir>
"""
import importlib.util
import os
import shutil
import sys
import tempfile
import warnings

FAMILY = "ga102"
BOOT_FILES = ("bootloader", "booter_load", "booter_unload")

# What main() passes for GA102: the size of each booter signature, and the part of the
# bootloader's array names that picks its production arrays rather than its debug ones
SIGNATURE_BYTES = 384
PRODUCTION_ARRAY_INFIX = "_prod_"


def load_tool(path):
    spec = importlib.util.spec_from_file_location("extract_firmware_nouveau", path)
    tool = importlib.util.module_from_spec(spec)

    # Python warns about the invalid escapes in two of the tool's regular expressions, which keep
    # their backslash and so still match as written
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", SyntaxWarning)
        spec.loader.exec_module(tool)

    return tool


def main():
    open_modules, tool_path, firmware_version, output_dir = sys.argv[1:]
    tool = load_tool(os.path.join(open_modules, tool_path))

    # The tool names its files after its module variable `version`, which main() sets to the
    # version's digits, and reads the generated files at paths relative to the working directory
    tool.version = firmware_version.replace(".", "")
    os.chdir(open_modules)

    with tempfile.TemporaryDirectory() as tool_output:
        tool.outputpath = tool_output
        tool.booter(FAMILY, "load", SIGNATURE_BYTES)
        tool.booter(FAMILY, "unload", SIGNATURE_BYTES)
        tool.bootloader(FAMILY, PRODUCTION_ARRAY_INFIX)

        os.makedirs(output_dir)
        for name in BOOT_FILES:
            written = os.path.join(tool_output, "nvidia", FAMILY, "gsp", f"{name}-{tool.version}.bin")
            shutil.move(written, os.path.join(output_dir, f"{name}.bin"))


if __name__ == "__main__":
    main()
