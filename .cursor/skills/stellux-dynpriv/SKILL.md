---
name: stellux-dynpriv
description: Decide whether kernel code or data needs hardware privilege, bracket elevation correctly, and verify a privilege change safely. Use when adding kernel code that touches hardware or allocates memory, when marking or unmarking __PRIVILEGED_CODE or __PRIVILEGED_DATA, when working with RUN_ELEVATED, or when auditing the privileged footprint.
---

# StelluxOS Dynamic Privilege

One kernel binary runs at two hardware privilege levels. Kernel threads execute
Ring 3 / EL0 inside the kernel address space and elevate only for work that
architecturally requires it. Shrinking privileged code and data is the point of
the project: a stray write from a deprivileged subsystem faults immediately with
a backtrace instead of corrupting kernel state silently.

## Architecture facts

These are verified against the code, rely on them instead of re-deriving:

- Regular kernel text is mapped `USER_RX`, rodata `USER_RO`, data, bss, and the
  per-CPU area `USER_RW`. Only `.priv.*` sections and the privileged heap are
  supervisor-only.
- SMEP and SMAP are detected but deliberately never enabled, so privileged code
  freely executes and reads user-mapped kernel memory.
- A lowered thread faults on: calling any `__PRIVILEGED_CODE` function (fetch
  from `.priv.text`), touching `.priv.*` or privileged-heap memory, privileged
  instructions, and all port I/O on x86 (IOPL is 0 and the TSS carries no I/O
  bitmap).
- Traps, syscalls, and IRQs always enter at Ring 0, and entry sets
  `percpu_is_elevated`, so handler bodies and everything they call run elevated.
- `elevate()` is a real syscall gated by `TASK_FLAG_CAN_ELEVATE`. `lower()` is
  inlined `sysretq` / `eret` and never leaves the current function.
- Unprivileged heap memory is reachable by lowered kernel threads but not by
  userland: `create_user_pt_root` copies the kernel half with the USER bit
  cleared, and x86 permissions AND across levels. aarch64 gets this from the
  TTBR0 and TTBR1 split.
- `.priv.bss` is a PROGBITS section, so privileged zero-initialized data
  occupies real bytes in the kernel image.
- `spin_lock` and `spin_unlock` are unprivileged. Only the `irqsave` variants
  are privileged, because they manipulate the interrupt flag.
- Logging elevates internally, so `log::` is safe to call from lowered code.

## Never leave a RUN_ELEVATED block early

The macro is a `do { ... } while (0)` that calls `lower()` after the body. A
`return`, `break`, `continue`, or `goto` inside the body skips that call and the
thread keeps hardware privilege permanently. Produce a value inside the block
and act on it afterwards. A `[[noreturn]]` call such as `sched::exit` is the
only exception, since the thread never resumes.

Also never write `RUN_ELEVATED` inside a function that is already
`__PRIVILEGED_CODE`, where it can only no-op.

## Deciding whether something needs privilege

Sort the item into one of three groups.

**Clearly privileged, leave it alone:** page table roots and page tables,
descriptor tables, syscall and interrupt dispatch tables, elevation state, task
structures, and the register bases from which elevated MMIO or port I/O is
derived. These are the machinery of privilege itself, and the syscall table in
particular is reachable by design from Ring 3, so corrupting it is directly
exploitable.

**Clearly unprivileged, keep it out:** driver objects and their callbacks,
device registers and DMA memory a driver touches while lowered, packet buffers,
protocol state, debug and symbol tables, string scratch, and anything holding
bytes that came from outside the kernel.

**Genuinely ambiguous:** filesystem nodes, socket state, resource objects. Weigh
bug density against criticality. High bug density and low criticality argues for
unprivileged, which is why drivers are an easy call. Core infrastructure that
everything depends on is a harder call, and leaving it privileged is defensible.

A settled ruling: subsystem state and callbacks may live in unprivileged memory
even though elevated code dispatches through them, because driver instances
already work that way by design. This does not extend to the transition
machinery listed above.

## Changing existing privilege

Demoting a **function** is safe when its body is genuinely pure. Verify by
reading the whole body including callees, and remember that a function can be
privileged through a parameter: a pointer into a task structure or the
privileged heap is privileged memory even though nothing in the signature says
so. Two audit passes misclassified functions for exactly this reason.

Demoting **data** cannot fault, since privileged code may always touch
unprivileged memory. The real hazard is allocator mismatch: convert every
allocation together with every matching free, or the slab magic check panics.
Note when a single shared free serves many allocation sites, which makes the
change all-or-nothing.

Promoting data is the opposite risk: any lowered reader that previously worked
will now fault, so check every accessor before moving something in.

Interface parity blocks unilateral changes. If a common header declares a
function privileged because one architecture needs it, the other architecture
cannot demote its implementation alone.

## Verifying a privilege change

1. `make -C kernel ARCH=x86_64` and `ARCH=aarch64`, both clean.
2. `scripts/dynpriv-lint.sh`, all checks green.
3. `make test ARCH=x86_64` and `ARCH=aarch64`.
4. Live boot, because unit tests never issue a syscall from a real Ring 3
   process. Confirm the userland shell prompt appears and the log holds no
   panic, page fault, or general protection fault.
5. For anything on a packet path, exercise real traffic rather than trusting a
   quiet boot. `ping 127.0.0.1` covers loopback and `ping 10.0.2.2` covers the
   NIC path under QEMU user networking.
6. Confirm placement rather than assuming it: `nm` the image and check the
   symbol falls inside or outside the privileged window.

`scripts/priv-footprint.sh` prints privileged text, data, and bss sizes plus
annotation and allocation counts. Record it before and after.

## Traps that have actually bitten

- A function that looks pure but dereferences a parameter pointing into the
  privileged heap.
- Removing `__PRIVILEGED_CODE` and leaving the `@note Privilege` line behind.
  Lint check 4 now catches this.
- Mixing `uzalloc` with `kfree`.
- Assuming an elevation bracket exists for register access when it actually
  exists for an `irqsave` lock or a scheduler call, which are separate reasons
  with separate fixes.
- Reading an audit report as ground truth. Verify each claim before acting.

## Deliberately deferred

Recorded so they are not rediscovered from scratch:

- Converting task-only `irqsave` locks to plain spinlocks would remove many
  elevation brackets, but plain spinlocks lose the no-preemption guarantee that
  disabling interrupts provides. That needs a `preempt_disable` primitive first,
  which would also close a preemption window in ktrace.
- Filesystem nodes stay privileged. Demoting the storage without also lowering
  the consumer paths would remove the stray-write tripwire while leaving the
  filesystem running at Ring 0, which is worse than either end state. Revisit
  only alongside deprivileging the leaves, since ramfs content lives in
  supervisor-only HHDM pages.
- Marking `.priv.bss` NOLOAD would reclaim image size with no runtime change.
