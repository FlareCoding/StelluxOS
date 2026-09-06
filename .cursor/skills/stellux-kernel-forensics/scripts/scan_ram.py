#!/usr/bin/env python3
"""Search guest RAM dumps for pages matching field conditions, or for a byte string.

Dumps come from `pmemsave START SIZE "file"`, so each file needs the guest
physical address it starts at: --dump FILE@BASE (BASE in hex).

Page mode: every condition must hold at the given offset within a page.
  scan_ram.py --dump ram_0x0.bin@0 --dump ram_0x100000000.bin@0x100000000 \
      --u32 0x0=0x534c4142 --u64 0x28=0xffff8100101a63a0 --show 0x20:16 --show 0x74:16

Find mode: locate a byte string anywhere, with a histogram of page offsets,
which tells whether hits sit at one structural offset (one object type) or not.
  scan_ram.py --dump ram_0x0.bin@0 --find 000102030405060708090a0b0c0d0e0f --show=-8:8

--show OFF:LEN prints LEN bytes at OFF relative to the page (page mode) or to
the hit (find mode, negative offsets need the --show=-8:8 form). Output is hex, little endian
integers are decoded for 4 and 8 byte fields. Runs over 4 GB in seconds.
"""
import argparse, mmap, struct, sys
from collections import Counter


def parse_dump(spec):
    path, base = spec.rsplit('@', 1)
    return path, int(base, 16)


def parse_cond(spec, size):
    off, val = spec.split('=', 1)
    return int(off, 16), int(val, 16), size


def fmt_field(mm, pos, length):
    raw = bytes(mm[pos:pos + length])
    if length == 8:
        return f'{struct.unpack("<Q", raw)[0]:#018x}'
    if length == 4:
        return f'{struct.unpack("<I", raw)[0]:#010x}'
    return raw.hex()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump', action='append', required=True, help='FILE@BASE_HEX')
    ap.add_argument('--page', type=lambda s: int(s, 0), default=4096)
    ap.add_argument('--u64', action='append', default=[], help='OFF=VAL, both hex')
    ap.add_argument('--u32', action='append', default=[], help='OFF=VAL, both hex')
    ap.add_argument('--bytes', action='append', default=[], help='OFF=HEXBYTES')
    ap.add_argument('--find', help='hex byte string to locate anywhere')
    ap.add_argument('--show', action='append', default=[], help='OFF:LEN to print per hit')
    ap.add_argument('--limit', type=int, default=200)
    args = ap.parse_args()

    conds = [parse_cond(c, 8) for c in args.u64] + [parse_cond(c, 4) for c in args.u32]
    byte_conds = []
    for c in args.bytes:
        off, val = c.split('=', 1)
        byte_conds.append((int(off, 16), bytes.fromhex(val)))
    shows = []
    for s in args.show:
        off, length = s.split(':')
        shows.append((int(off, 0), int(length, 0)))
    if not conds and not byte_conds and not args.find:
        sys.exit('give at least one --u64/--u32/--bytes condition or --find')

    hits = 0
    for spec in args.dump:
        path, base = parse_dump(spec)
        with open(path, 'rb') as f:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if args.find:
                needle = bytes.fromhex(args.find)
                offs = []
                pos = mm.find(needle)
                while pos != -1 and len(offs) < 1_000_000:
                    offs.append(pos)
                    pos = mm.find(needle, pos + 1)
                print(f'{path}: {len(offs)} occurrences of {args.find}')
                hist = Counter(o % args.page for o in offs)
                print('  page-offset histogram:', ', '.join(f'{o:#x}:{n}' for o, n in sorted(hist.items())[:32]))
                for o in offs[:args.limit]:
                    fields = ' '.join(f'[{so:+#x}]={fmt_field(mm, o + so, ln)}' for so, ln in shows if 0 <= o + so < len(mm))
                    print(f'  gpa={base + o:#x} pageoff={o % args.page:#x} {fields}')
                hits += len(offs)
            else:
                for i in range(len(mm) // args.page):
                    p = i * args.page
                    ok = True
                    for off, val, size in conds:
                        fmt = '<Q' if size == 8 else '<I'
                        if struct.unpack_from(fmt, mm, p + off)[0] != val:
                            ok = False
                            break
                    if ok:
                        for off, val in byte_conds:
                            if bytes(mm[p + off:p + off + len(val)]) != val:
                                ok = False
                                break
                    if not ok:
                        continue
                    hits += 1
                    if hits <= args.limit:
                        fields = ' '.join(f'[{so:#x}]={fmt_field(mm, p + so, ln)}' for so, ln in shows)
                        print(f'gpa={base + p:#x} {fields}')
            mm.close()
    print(f'total hits: {hits}')


if __name__ == '__main__':
    main()
