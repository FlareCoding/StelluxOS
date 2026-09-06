# Forensics reference

Cookbook material for the stellux-kernel-forensics skill. Read the section you
need, the SKILL.md workflow says when.

## Decoding a panic dump

### x86_64 page fault

- `CR2` is the address the instruction touched, not the instruction. A small
  CR2 such as `0x8` is `nullptr + field offset`: find which field sits at that
  offset in the struct being touched (see "struct layouts" below).
- Error code bits: 1 = protection violation (0 = not present), 2 = write,
  4 = user mode, 8 = reserved bit set, 16 = instruction fetch.
- The trap frame registers are the faulting instruction's registers. In the
  default `-O0 -g` build the System V conventions hold literally: the first six
  integer arguments arrive in `rdi, rsi, rdx, rcx, r8, r9`, `this` is `rdi`,
  and parameters are spilled to `-8(%rbp)`, `-16(%rbp)`, ... at function entry.
  Registers that are not operands of the faulting instruction are leftovers
  from earlier code and mean nothing. Do not build a story on them.
- Name the exact load or store:

```bash
llvm-objdump -d --no-show-raw-insn \
    --start-address=0x<rip-0x30> --stop-address=0x<rip+0x10> build/kernel/x86_64/kernel.elf
```

  Use the Homebrew LLVM binary on macOS, `/opt/homebrew/opt/llvm/bin/llvm-objdump`.

### aarch64 data abort

- `FAR` is the address touched, `ESR` carries `EC` (0x24 from EL0, 0x25 same EL)
  and `DFSC` (0x04-0x07 translation fault by level, 0x0d-0x0f permission
  fault, 0x21 alignment). A translation fault on a kernel heap address means
  the page is unmapped, which after a `vmm::free` is the expected UAF symptom.
- Arguments arrive in `x0..x7`, `this` in `x0`, spilled to the frame at `-O0`.

### Struct layouts without guessing

```bash
gdb -batch -ex 'file build/kernel/x86_64/kernel.elf' -ex 'ptype /o net::packet'
```

`ptype /o` prints every field with its offset and size. This is how a
`nullptr + 0x8` write becomes "the `next` pointer of `list::node`".

## What freed memory looks like

Verify against `kernel/mm/heap.cpp` and `heap_internal.h` before relying on it:

- Size classes are 16..2048 bytes, one slab per 4 KB page with a 32 byte
  header, so a 2048 byte object is alone on its page and every alloc or free of
  it is a page map or unmap through `vmm`.
- `free_internal` writes the freelist pointer at offset 0 of the object. For a
  one-object slab that pointer is null, so a freed object reads as zero at
  offset 0. With `DEBUG` the rest is poisoned with `0xDE` first.
- `uzalloc` and `kzalloc` zero the object, `ALLOC_ZERO` pages are zeroed
  through the direct map, and `alloc_internal` zeroes slab objects under
  `DEBUG`. A field that reads as zero where a pointer belongs therefore means
  "never written through the current mapping" as often as it means "freed".
- A slab page whose header magic reads as zero while an object on it is
  referenced by live data is impossible under a coherent view of memory:
  `new_slab` writes the magic before returning the first object.

## gdb against the QEMU stub

```bash
gdb -batch -q -ex 'file build/kernel/x86_64/kernel.elf' -ex 'target remote :4554' \
    -ex 'info threads' -ex 'thread apply all bt 12'
```

- Thread N is CPU N-1. `info threads` shows which CPUs are `halted`.
- Every memory read (`x`, `print *p`, `dump`) goes through a page-table walk of
  the selected thread's CR3 or TTBR. It never consults the CPU's TLB. That is
  what makes gdb the right tool to prove a stale translation: compare what the
  CPU reported in the trap frame with what gdb reads at the same address.
- `print/x $cr3`, `$cr4`, `$cr2` work on x86. Read them per thread with
  `thread apply all -q -s print/x $cr3`.
- `frame function NAME` selects the frame by function. The unwinder sometimes
  stops at the trap entry stub; then take pointers such as `this=` and the
  arguments from the `bt` text instead.
- Breakpoints are written into guest memory, so `-S` plus `break panic::on_trap`
  plus `continue` freezes every CPU the instant any of them panics. Without
  this only the faulting CPU halts, the others keep running and mutate memory.
- When gdb exits, it detaches and the VM resumes. Collect everything, including
  monitor commands, inside the same gdb session.
- One gdb client at a time. A second attach fails silently.
- gdb built by Homebrew has Python: `python import gdb; v=int(gdb.parse_and_eval('tf->rdi')); gdb.execute('monitor gva2gpa %#x' % v)`
  evaluates expressions and feeds them to monitor commands.

## HMP monitor cookbook

Through gdb as `monitor <cmd>`, or through a socket started with
`-monitor unix:/tmp/mon,server,nowait` as `printf 'cmd\n' | nc -U /tmp/mon`.

- `gva2gpa ADDR` translates through the current CPU's page tables (walk, not TLB).
- `x /8gx VADDR` reads through the page walk, `xp /8gx PADDR` reads physical memory.
- `info registers -a` dumps all CPUs, `info tlb` and `info mem` print the page
  table contents (despite the name, `info tlb` is a page-table walk).
- `pmemsave START SIZE "FILE"`: the path must be double quoted, otherwise the
  expression parser reads `/tmp` as a division and fails with
  `invalid char 't' in expression`.
- `stop` and `cont` pause and resume, `info status` shows the run state.
- `sendkey`, `mouse_move`, `screendump` are covered in the stellux-qemu-testing skill.

## Guest RAM layout

- q35 with `-m 4G`: RAM is `0x0-0x80000000` and `0x100000000-0x180000000`.
  The high frames are where the kernel heap usually lands.
- virt (aarch64) with `-m 4G`: RAM is `0x40000000-0x140000000`.
- Kernel image at `0xffffffff80000000`, kernel heap and kva ranges observed at
  `0xffff8100xxxxxxxx`, the direct map (`phys_to_virt`) below that. Confirm with
  `info mem` rather than assuming.
- A `pmemsave` of 2 GB takes a few seconds and 2 GB of disk. Delete dumps when done.

## Reading run statistics

- Intermittent means a rate. Establish the baseline rate with the same command,
  image, `-smp`, and run count you will use for the comparison, ideally in the
  same session so host load is comparable.
- 0 of 16 against 7 of 16 is evidence (Fisher exact p about 0.003). 0 of 3
  against 1 of 3 is noise. Double the clean sample before calling a fix done.
- A run that never reached the shell prompt is neither clean nor failed, it is
  a harness problem. The scripts report it as `booted=0`, investigate it first.
