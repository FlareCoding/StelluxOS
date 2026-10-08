"""Builds a lookup database naming the raw codes in a ktrace-decode database.

Usage: python3 build_lookups.py RUN_DB LOOKUP_DB

Reads the architecture from RUN_DB's session table, then writes these tables to
LOOKUP_DB, each parsed from the source that defines the codes:

  syscall_names(number, name)  musl's bits/syscall.h for that architecture, and
                               libstlx's stlx/syscall_nums.h for StelluxOS syscalls
  errno_names(errno, name)     musl's bits/errno.h, Linux numbering
  task_kinds(kind, name)       TASK_KIND_* in kernel/trace/ktrace_format.h
  switch_reasons(reason, name) SWITCH_REASON_* in kernel/trace/ktrace_format.h
  fault_flags(bit, name)       PF_FLAG_* in kernel/mm/mm.h
  fault_results(result, name)  MM_CTX_* in kernel/mm/vma.h

Queries then run `ATTACH 'LOOKUP_DB' AS lookup` and join lookup.syscall_names and so on.
"""
import re
import sqlite3
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]


def read(relative_path):
    path = REPO / relative_path
    if not path.exists():
        sys.exit(f"build_lookups: {path} is missing. Build that architecture's sysroot with `make musl ARCH=...`.")

    return path.read_text()


def syscall_names(arch):
    musl_header = read(f'userland/sysroot/{arch}/include/bits/syscall.h')
    stellux_header = read('userland/lib/libstlx/include/stlx/syscall_nums.h')

    names = {int(number): name for name, number in re.findall(r'#define __NR_(\w+)\s+(\d+)', musl_header)}
    for name, number in re.findall(r'#define SYS_(\w+)\s+(\d+)', stellux_header):
        names[int(number)] = name.lower()

    return names


def errno_names(arch):
    names = {}
    for name, number in re.findall(r'#define (E\w+)\s+(\d+)', read(f'userland/sysroot/{arch}/include/bits/errno.h')):
        names.setdefault(int(number), name)

    return names


def format_constants(prefix):
    header = read('kernel/trace/ktrace_format.h')
    pattern = rf'constexpr uint8_t {prefix}_(\w+)\s*=\s*(\d+);'
    return {int(number): name.lower() for name, number in re.findall(pattern, header)}


def fault_flags():
    pattern = r'PF_FLAG_(\w+)\s*=\s*\(1u << (\d+)\)'
    return {1 << int(bit): name.lower() for name, bit in re.findall(pattern, read('kernel/mm/mm.h'))}


def fault_results():
    pattern = r'constexpr int32_t (MM_CTX_\w+)\s*=\s*(-?\d+);'
    results = {int(number): name for name, number in re.findall(pattern, read('kernel/mm/vma.h'))}
    results.setdefault(0, 'MM_CTX_OK')

    return results


def main(run_db, lookup_db):
    arch = sqlite3.connect(run_db).execute('SELECT arch FROM session').fetchone()[0]
    if arch not in ('x86_64', 'aarch64'):
        sys.exit(f"build_lookups: {run_db} records an architecture this script has no tables for: {arch}")

    tables = {
        'syscall_names': ('number', syscall_names(arch)),
        'errno_names': ('errno', errno_names(arch)),
        'task_kinds': ('kind', format_constants('TASK_KIND')),
        'switch_reasons': ('reason', format_constants('SWITCH_REASON')),
        'fault_flags': ('bit', fault_flags()),
        'fault_results': ('result', fault_results()),
    }

    db = sqlite3.connect(lookup_db)
    for table, (key, rows) in tables.items():
        db.execute(f'DROP TABLE IF EXISTS {table}')
        db.execute(f'CREATE TABLE {table} ({key} INTEGER PRIMARY KEY, name TEXT NOT NULL)')
        db.executemany(f'INSERT INTO {table} VALUES (?, ?)', sorted(rows.items()))
        print(f"{table}: {len(rows)} rows")

    db.commit()
    print(f"wrote {lookup_db} for {arch}")


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2])

    main(sys.argv[1], sys.argv[2])
