# Runs inside gdb (`source frozen_evidence.py`) while the VM is frozen at the
# panic entry. Reconstructs the faulting context's stack trace from the trap
# frame, then dumps every kernel address held in the fault registers through a
# page-table walk and translates it with gva2gpa. Reads nothing from the serial
# log, because at the panic entry nothing has been printed yet.
#
# Set the gdb convenience variable $arch to "x86_64" or "aarch64" before sourcing.
import gdb

KERNEL_HALF = 0xffff800000000000
MASK = 0xffffffffffffffff


def ev(expr):
    try:
        return int(gdb.parse_and_eval(expr)) & MASK
    except gdb.error:
        return None


def out(text):
    gdb.write(text + "\n")


def symbolize(addr):
    sym = gdb.execute(f"info symbol {addr:#x}", to_string=True).strip()
    try:
        line = gdb.execute(f"info line *{addr:#x}", to_string=True).strip().split("\n")[0]
    except gdb.error:
        line = ""
    return f"{sym}  {line}"


def walk_frames(fp, pc, depth=24):
    out(f"  #0  {pc:#018x}  {symbolize(pc)}")
    for i in range(1, depth):
        ret = ev(f"*(unsigned long*)({fp:#x} + 8)")
        nxt = ev(f"*(unsigned long*)({fp:#x})")
        if ret is None or ret < KERNEL_HALF:
            break
        out(f"  #{i}  {ret:#018x}  {symbolize(ret)}")
        if nxt is None or nxt == 0 or nxt <= fp:
            break
        fp = nxt


def dump_kernel_address(name, addr):
    if addr is None:
        return
    if addr < KERNEL_HALF:
        out(f"\n=== {name} = {addr:#x}: not a kernel address, skipped ===")
        return
    page = addr & ~0xfff
    out(f"\n=== {name} = {addr:#x} via page walk (not the TLB) ===")
    gdb.execute(f"x/8gx {addr:#x}")
    out(f"--- page {page:#x}: first 32 bytes and translation ---")
    gdb.execute(f"x/4gx {page:#x}")
    gdb.execute(f"monitor gva2gpa {page:#x}")


arch = str(gdb.parse_and_eval("$arch")).strip('"')

out("\n=== faulting context stack (frame-pointer walk from the trap frame) ===")
if arch == "x86_64":
    walk_frames(ev("tf->rbp"), ev("tf->rip"))
    regs = [("cr2", ev("$cr2")), ("tf->rdi", ev("tf->rdi")), ("tf->rsi", ev("tf->rsi")),
            ("tf->rax", ev("tf->rax")), ("tf->rdx", ev("tf->rdx"))]
else:
    walk_frames(ev("tf->x[29]"), ev("tf->elr"))
    regs = [("far", ev("tf->far")), ("tf->x[0]", ev("tf->x[0]")), ("tf->x[1]", ev("tf->x[1]")),
            ("tf->x[8]", ev("tf->x[8]"))]

seen = set()
for name, addr in regs:
    if addr is None or (addr & ~0xfff) in seen:
        continue
    seen.add(addr & ~0xfff)
    dump_kernel_address(name, addr)
