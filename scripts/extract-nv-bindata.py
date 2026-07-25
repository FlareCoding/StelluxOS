#!/usr/bin/env python3
"""Extract and decompress a firmware blob from an NVIDIA open-gpu-kernel-modules
generated bindata source file.

Background
----------
open-gpu-kernel-modules embeds several GPU microcodes (the GSP-RM RISC-V boot
"SK+BL" image and its descriptor, the SEC2 Booter Load/Unload ucodes, ...) as
"bindata": auto-generated C byte arrays in src/nvidia/generated/g_bindata_*.c.
Each storage entry is raw-DEFLATE-compressed (RFC 1951 - no zlib/gzip wrapper);
this matches the in-driver decompressor utilGzIterator()
(src/nvidia/src/lib/zlib/inflate.c), which reads the DEFLATE BFINAL bit and the
2-bit block type directly. Verified against version.mk = 535.183.01.

Rather than port a decompressor into the StelluxOS kernel, we decompress here on
the host and stage the raw bytes as plain files in the initrd, exactly like
gsp_ga10x.bin. The kernel then just loads the raw image / descriptor.

Usage
-----
    extract-nv-bindata.py --in <g_bindata_*.c> --storage <name> --out <file.bin>

Example (GSP-RM boot SK+BL image + descriptor for GA102, production):
    extract-nv-bindata.py \\
        --in .../generated/g_bindata_kgspGetBinArchiveGspRmBoot_GA102.c \\
        --storage ucode_image_prod --out gsp_ga10x_boot_image.bin
"""
import argparse
import re
import sys
import zlib


def _find_block(text: str, storage: str):
    """Return (compressed_bytes, declared_data_size, is_compressed) for the
    bindata storage whose data array variable ends with '<storage>_data'."""
    arr_re = re.compile(r"NvU8\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\}\s*;", re.S)
    target = storage + "_data"
    for m in arr_re.finditer(text):
        if not m.group(1).endswith(target):
            continue
        body = m.group(2)
        raw = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{1,2})", body))

        # The metadata comment block immediately precedes this array; take the
        # nearest preceding DATA SIZE / COMPRESSION values.
        head = text[: m.start()]
        sizes = re.findall(r"DATA SIZE \(bytes\):\s*(\d+)", head)
        comps = re.findall(r"COMPRESSION:\s*(\w+)", head)
        if not sizes or not comps:
            sys.exit(f"error: could not find metadata for storage '{storage}'")
        return raw, int(sizes[-1]), comps[-1].strip().upper() == "YES"
    sys.exit(f"error: storage '{storage}' (variable *_{target}) not found")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", required=True, help="generated g_bindata_*.c file")
    ap.add_argument("--storage", required=True, help="storage name, e.g. ucode_image_prod")
    ap.add_argument("--out", required=True, help="output raw .bin path")
    args = ap.parse_args()

    with open(args.src, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    comp, data_size, is_comp = _find_block(text, args.storage)

    if is_comp:
        # wbits=-15 -> raw DEFLATE, no zlib/gzip header (matches utilGzIterator).
        raw = zlib.decompress(comp, -15)
    else:
        raw = comp

    if len(raw) != data_size:
        sys.exit(f"error: decompressed {len(raw)} bytes, expected {data_size} "
                 f"(declared DATA SIZE) for storage '{args.storage}'")

    with open(args.out, "wb") as f:
        f.write(raw)

    print(f"{args.storage}: compressed={len(comp)} -> raw={len(raw)} bytes "
          f"(compressed={is_comp}) -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
