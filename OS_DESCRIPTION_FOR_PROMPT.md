# Stellux 3.0 — Detailed OS Description for LLM Context

## Project Overview

Stellux 3.0 is a **bare-metal monolithic kernel** written entirely in **C++20 (freestanding, no standard library)**. It targets **x86_64** and **AArch64** architectures, boots via UEFI using the **Limine v8+ bootloader**, and is built with the **LLVM/Clang** toolchain. The kernel is in a prototype/development stage — it has a working memory management subsystem, a preemptive round-robin task scheduler, interrupt handling, system calls, and a novel "dynamic privilege" mechanism, but does **not** yet have userspace programs, a filesystem, networking, or most device drivers.

---

## Architecture Support

The kernel is fully dual-architecture:

- **x86_64**: Uses SYSCALL/SYSRET for system calls, LAPIC for interrupts and timers, GDT for segmentation, MSRs for system call configuration. Compiler target `x86_64-unknown-none` with flags `-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mcmodel=kernel`.
- **AArch64**: Uses SVC/ERET for system calls, GICv2 for interrupts, the ARM Generic Timer for hardware timers, and VBAR for exception vectors. Compiler target `aarch64-unknown-none` with `-mgeneral-regs-only`.

Both architectures share a common kernel codebase (`kernel/`) with architecture-specific code isolated in `kernel/arch/x86_64/` and `kernel/arch/aarch64/`. Maximum supported CPUs is 64 (configurable via `MAX_CPUS`), though SMP is not fully implemented yet (per-CPU data infrastructure exists but Application Processor startup is not yet done).

---

## Build System

GNU Make with LLVM/Clang toolchain:

- `make kernel ARCH=x86_64|aarch64` — build the kernel ELF
- `make image ARCH=...` — create a UEFI-bootable disk image (GPT + FAT32 ESP)
- `make run ARCH=...` — build and run in QEMU with OVMF/QEMU-EFI firmware
- `make test ARCH=...` — build with unit tests enabled, run in QEMU

The build auto-discovers all `.cpp` and `.S` files recursively, compiles with architecture-specific flags from `kernel/arch/*/defconfig`, and links with `ld.lld` using architecture-specific linker scripts. Dependencies include `clang++`, `ld.lld`, QEMU, OVMF, `mtools`, and `gdisk`.

C++20 is used in freestanding mode with `-ffreestanding -nostdlib -nostdinc++ -fno-exceptions -fno-rtti`. All standard library types (integers, size_t, nullptr) are manually defined in `kernel/common/types.h`.

---

## Boot Process

1. **Limine** loads the kernel ELF into memory, sets up identity mapping + HHDM (Higher Half Direct Map), and jumps to `_start` in the `.priv.text` section.
2. `_start` (architecture-specific assembly) sets up the initial stack, masks interrupts, and calls `stlx_init()`.
3. `stlx_init()` (C++ entry point in `kernel/boot/boot.cpp`) runs the initialization sequence:
   - `boot_services::init()` — parse Limine boot protocol responses (memory map, RSDP, HHDM offset, kernel address)
   - `serial::init()` — initialize UART for logging
   - `arch::early_init()` — architecture-specific early setup (GDT, trap vectors, etc.)
   - `mm::init()` — full memory management initialization (PMM → VA layout → KVA → VMM → heap → early MMU)
   - `acpi::init()` — parse ACPI tables (RSDP → XSDT → MADT)
   - `irq::init()` — initialize interrupt controllers (LAPIC / GICv2)
   - `sched::init()` — initialize the scheduler, create idle task, set up per-CPU runqueues
   - `hwtimer::init(100)` — start the hardware timer at 100Hz for preemption
4. After init, demo kernel tasks are created (e.g., a Fibonacci computation task) and the boot CPU enters a halt loop.

---

## Memory Management

The MM subsystem is the most complete part of the kernel, consisting of five tightly integrated components:

### Physical Memory Manager (PMM)
- **Buddy allocator** with free lists, supporting orders 0 (4KB) through 18 (1GB).
- **Two zones**: DMA32 (0–4GB) and NORMAL (4GB+), with zone-aware allocation via `zone_mask_t`.
- **Page frame descriptors**: 16-byte `page_frame_descriptor` structs, one per physical page, storing flags (ALLOCATED, RESERVED, HUGE_HEAD, HUGE_TAIL, SLAB) and an order field.
- **Bootstrap allocator**: A simple bump allocator used during early boot before the buddy allocator is ready, for allocating page tables.
- API: `alloc_pages(order, zones)`, `free_pages(addr, order)`, `alloc_page()`, `free_page()`, `get_page_frame()`, `free_page_count()`.

### Virtual Address Layout
- Computed once from boot info and linker symbols.
- Regions: HHDM (direct physical map, base from Limine, typically `0xffff800000000000`), KVA-managed range (between HHDM end and kernel image), kernel image (`0xffffffff80000000`+).

### Kernel Virtual Address (KVA) Allocator
- Manages the virtual address range between HHDM and the kernel image.
- Uses a **red-black tree** of free ranges for allocation.
- Supports low placement (heaps, boot structures) and high placement (stacks, growing downward).
- Features: guard pages (pre/post), alignment, tagging for debugging, coalescing on free.

### Virtual Memory Manager (VMM)
- Allocates virtual address ranges via KVA, then backs them with physical pages and page table mappings.
- Allocation types:
  - **Non-contiguous**: always 4KB pages, each allocated independently.
  - **Contiguous**: supports 2MB and 1GB large pages for large allocations.
  - **Stacks**: allocated with guard pages (pre and post).
  - **Device mappings**: MMIO regions with uncacheable memory attributes.
  - **Physical mappings**: maps specific physical ranges (for firmware tables, etc.).
- Handles page table creation/population for each allocation.

### Heap Allocator
- **Slab allocator** with 8 size classes for small allocations, plus direct VMM-backed large allocations.
- **Two heaps**:
  - Privileged heap (`kalloc`/`kfree`/`kzalloc`): pages mapped with `PAGE_KERNEL_RW`, only accessible at Ring 0/EL1.
  - Unprivileged heap (`ualloc`/`ufree`/`uzalloc`): pages mapped with `PAGE_USER_RW`, accessible at any privilege level.
- C++ `new`/`delete` wrappers: `kalloc_new<T>()`, `kfree_delete<T>()`, `ualloc_new<T>()`, `ufree_delete<T>()`.
- Zero-initialization variants available.
- Auto-elevation: unprivileged heap functions automatically elevate privilege when called from Ring 3/EL0.

### Paging
- **x86_64**: 4-level page tables (PML4 → PDPT → PD → PT). Supports 4KB, 2MB, and 1GB page sizes.
- **AArch64**: 4-level page tables (L0 → L1 → L2 → L3).
- Page flags: READ, WRITE, EXEC, USER, GLOBAL, LARGE_2MB, HUGE_1GB, memory types (NORMAL/WB, DEVICE/uncached, WC).
- Kernel is mapped in the higher half at `0xffffffff80000000`.

---

## Task Scheduler

### Task Model
Only kernel tasks exist (no userspace processes yet). Each task has:

```cpp
struct task {
    task_exec_core exec;      // execution context (MUST be at offset 0 for assembly)
    uint32_t       tid;       // unique task ID
    uint32_t       state;     // READY, RUNNING, or DEAD
    list::node     sched_link; // intrusive linked list node for runqueue
    const char*    name;      // debug name
};
```

The `task_exec_core` contains:
- `flags` — bitmask of TASK_FLAG_ELEVATED, TASK_FLAG_KERNEL, TASK_FLAG_CAN_ELEVATE, TASK_FLAG_IDLE, TASK_FLAG_RUNNING, TASK_FLAG_IN_SYSCALL, TASK_FLAG_IN_IRQ, TASK_FLAG_PREEMPTIBLE, TASK_FLAG_FPU_USED
- `cpu` — which CPU this task is running on
- `task_stack_top` — the task's main stack
- `system_stack_top` — separate system/exception stack
- `cpu_ctx` — architecture-specific saved register context (`thread_cpu_context`)

### Scheduling Policy
- **Pluggable policy interface** via virtual `sched_policy` base class with methods: `enqueue()`, `dequeue()`, `pick_next()`, `tick()`.
- **Current implementation**: Round-robin (`round_robin_policy`) using an intrusive linked list of ready tasks.
- **Per-CPU runqueue** with a spinlock.
- **Preemption**: Timer interrupt (100Hz) calls `sched::tick()` which can trigger a context switch.
- **Idle task**: One per CPU, runs when no other tasks are ready.
- **Context switch**: Architecture-specific assembly that saves/restores callee-saved registers.

### Task Lifecycle
- Created via `sched::create_kernel_task(entry_fn, arg, name)`.
- Stacks allocated with guard pages via VMM.
- Tasks can be elevated (Ring 0/EL1) or lowered (Ring 3/EL0).
- Task states: READY → RUNNING → DEAD (no blocking/sleeping yet).
- `sched::exit()` marks a task as DEAD.

---

## Dynamic Privilege (dynpriv) — Novel Feature

This is the kernel's most distinctive architectural feature. Instead of the traditional model where kernel code always runs at Ring 0 and user code at Ring 3, Stellux allows tasks to **dynamically transition between privilege levels** at runtime:

- `dynpriv::elevate()` — transition from Ring 3/EL0 to Ring 0/EL1 (via SYSCALL on x86_64, SVC on AArch64)
- `dynpriv::lower()` — transition from Ring 0/EL1 to Ring 3/EL0 (via SYSRET on x86_64, ERET on AArch64)
- `dynpriv::is_elevated()` — check current privilege level
- `RUN_ELEVATED({ code })` — convenience macro that elevates, runs code, then lowers back

### Privileged Sections
Code and data are placed in privileged sections (`.priv.text`, `.priv.rodata`, `.priv.data`, `.priv.bss`) using section attribute macros:
- `__PRIVILEGED_CODE` — function in `.priv.text`
- `__PRIVILEGED_DATA` — data in `.priv.data`
- `__PRIVILEGED_RODATA` — read-only data in `.priv.rodata`
- `__PRIVILEGED_BSS` — zero-initialized data in `.priv.bss`

These sections are mapped as kernel-only (no USER flag), so lowered (Ring 3/EL0) tasks cannot access them. This creates a security boundary within the kernel itself — kernel tasks can run at reduced privilege, and only elevate when they need to access privileged resources.

### VS Code Extension
A custom VS Code extension (`tools/vscode-dynpriv/`) provides visual decoration for files/functions using privileged sections, helping developers track the privilege boundary in the codebase.

---

## Interrupt Handling

- **x86_64**: LAPIC-based. Legacy 8259 PIC is masked. LAPIC is mapped via HHDM. EOI via LAPIC EOI register. Timer interrupt on vector 0x20.
- **AArch64**: GICv2. Distributor and CPU interface mapped via MMIO. Supports PPIs (timer) and SPIs. EOI via GICC_EOIR. Timer interrupt on PPI 27.
- **Trap handling**: x86_64 uses IDT with 256 entries. AArch64 uses VBAR with exception vector table.
- **IRQ abstraction**: Common `irq::init()` and `irq::eoi()` interface across architectures.

---

## System Calls

Two system calls currently implemented:
- `SYS_YIELD` (1000) — yield CPU to scheduler
- `SYS_ELEVATE` (1001) — elevate privilege (used by `dynpriv::elevate()`)

Entry mechanism:
- **x86_64**: SYSCALL instruction → `stlx_x86_syscall_entry` (assembly) → `stlx_syscall_handler()` (C++). MSRs STAR, LSTAR, CSTAR, SFMASK configured during init. Arguments in RDI, RSI, RDX, R10, R8, R9.
- **AArch64**: SVC instruction → exception vector → `stlx_aarch64_syscall_dispatch()`. Syscall number from ESR_EL1 SVC immediate field. Arguments in x0–x5.

---

## Synchronization

- **Ticket spinlock**: Fair FIFO spinlock with `{next_ticket, now_serving}` (16-bit each). Cache-aligned (64 bytes). Uses `cpu::relax()` for backoff and `cpu::send_event()` for wakeup.
- Operations: `spin_lock()`, `spin_unlock()`, `spin_lock_irqsave()`, `spin_unlock_irqrestore()`.
- IRQ management: `cpu::irq_disable()`, `cpu::irq_enable()`, `cpu::irq_save()`, `cpu::irq_restore()`.

---

## Per-CPU Data

Template-based per-CPU variable system:
- `DEFINE_PER_CPU(type, name)` — define a per-CPU variable (in `.bss.percpu` section)
- `DECLARE_PER_CPU(type, name)` — forward-declare a per-CPU variable
- `this_cpu(var)` — access the current CPU's copy of the variable
- Cache-aligned variants available (`.bss.percpu..cacheline_aligned` section)
- Hard limit of 8KB per CPU, enforced by linker script assertions.

Key per-CPU variables include `current_task_exec` (pointer to the currently running task's execution core) and `percpu_is_elevated` (dynamic privilege state).

---

## Intrusive Data Structures

Two custom, allocation-free data structures:
- **Intrusive doubly-linked list** (`list::head<T, &T::node_member>`): type-safe, O(1) insert/remove.
- **Intrusive red-black tree** (`rbt::tree<T, Key, Comparator>`): type-safe, O(log n) insert/find/remove. Used by the KVA allocator for free range tracking.

---

## I/O and Device Drivers

Minimal driver support:
- **Serial port**: x86_64 uses COM1 (port I/O at 0x3F8), AArch64 uses PL011 UART (MMIO). Used exclusively for kernel logging.
- **MMIO helper**: `mmio::read<T>(addr)`, `mmio::write<T>(addr, val)` with proper volatile semantics.

**Not implemented**: block devices, network interfaces, graphics/framebuffer, USB, PCI/PCIe enumeration, or any ACPI device beyond MADT parsing.

---

## ACPI Support

- RSDP detection (from Limine boot info)
- XSDT/RSDT parsing with generic table lookup by 4-byte signature
- MADT parsing for CPU/interrupt controller topology (LAPIC entries on x86_64, GIC entries on AArch64)
- No DSDT/SSDT/AML interpreter, no power management, no other table handling.

---

## Logging

- Backend: serial UART
- Levels: DEBUG (0), INFO (1), WARN (2), ERROR (3), FATAL (4, halts system), NONE (5)
- **Compile-time filtering**: log calls below the configured `LOG_LEVEL` threshold are eliminated entirely by the compiler (zero overhead).
- Printf-style formatted output.

---

## Unit Testing

Custom in-kernel test framework (`stlx_unit_test`):
- Test descriptors placed in a `.stlx_unit_test` linker section.
- Test hooks in `.stlx_unit_test_hooks` section.
- Zero overhead when disabled (sections are empty).
- `stlx_test::run_all()` runs all registered tests.
- Tests cover: heap, VMM, KVA, PMM, paging, scheduler preemption, list, red-black tree.
- Enabled via `make test ARCH=...` which defines `STLX_UNIT_TESTS_ENABLED`.

---

## What Has NOT Been Implemented Yet

1. **Userspace**: No user processes, no ELF loader, no user virtual address spaces
2. **Filesystem**: No VFS, no block device drivers, no filesystem implementations
3. **Networking**: No network stack, no network drivers
4. **SMP**: Per-CPU infrastructure exists but Application Processor startup is not implemented
5. **IPC**: No inter-process/inter-task communication mechanisms
6. **Blocking/Sleeping**: Tasks can only be READY, RUNNING, or DEAD — no wait queues or sleep
7. **Virtual Memory per-process**: Single kernel address space, no per-task page tables
8. **Signal handling**: No signals or asynchronous event delivery
9. **PCI/PCIe**: No bus enumeration or PCI device drivers
10. **Graphics**: No framebuffer driver (Limine provides framebuffer info but it's unused)
11. **Power management**: No ACPI S-states, no frequency scaling
12. **DMA**: No DMA engine or IOMMU support

---

## Project Structure Summary

```
/workspace/
├── Makefile                     # Top-level build orchestration
├── config.mk                    # Shared configuration (MAX_CPUS, LOG_LEVEL, etc.)
├── boot/limine/                 # Limine bootloader config and binaries
├── kernel/
│   ├── Makefile                 # Kernel build rules (auto-discovery, linking)
│   ├── arch/
│   │   ├── x86_64/             # x86_64: linker.ld, start.S, defconfig, trap/, syscall/,
│   │   │                       #   sched/, mm/, irq/, acpi/, dynpriv/, gdt/, hw/, hwtimer/
│   │   └── aarch64/            # AArch64: linker.ld, start.S, defconfig, trap/, syscall/,
│   │                           #   sched/, mm/, irq/, acpi/, dynpriv/, hw/, hwtimer/, cpu/
│   ├── boot/                   # Boot init (boot.cpp = stlx_init entry, boot_services)
│   ├── common/                 # types.h, logging, string, list, rb_tree, limine.h, cxx_abi
│   ├── mm/                     # PMM, VMM, KVA, heap, paging, early_mmu, va_layout
│   ├── sched/                  # Scheduler, task, task_exec_core, sched_policy, runqueue
│   ├── syscall/                # System call dispatcher and entry points
│   ├── irq/                    # IRQ controller abstraction
│   ├── trap/                   # Exception/trap initialization
│   ├── acpi/                   # ACPI table parsing (RSDP, XSDT, MADT)
│   ├── percpu/                 # Per-CPU data infrastructure
│   ├── dynpriv/                # Dynamic privilege API
│   ├── sync/                   # Ticket spinlock
│   ├── io/                     # Serial port driver
│   ├── hw/                     # MMIO helpers
│   └── tests/                  # Unit tests (framework + memory/sched/common tests)
├── scripts/                    # Build helpers, GDB scripts, test runner
└── tools/vscode-dynpriv/       # VS Code extension for dynpriv visualization
```

---

## Design Philosophy and Key Decisions

1. **C++20 freestanding**: Leverages modern C++ features (templates, constexpr, attributes, namespaces) while maintaining full control over the runtime environment. No exceptions, RTTI, or standard library.
2. **Dual-architecture from day one**: All subsystems have both x86_64 and AArch64 implementations, with shared logic in common code.
3. **Dynamic privilege**: Rather than a hard kernel/user boundary, tasks can fluidly move between privilege levels. This enables security-conscious design where kernel tasks run at reduced privilege by default and only elevate when accessing protected resources.
4. **Intrusive data structures**: All kernel data structures (lists, trees) are intrusive (nodes embedded in the containing struct), avoiding allocation overhead and external node management.
5. **Privileged section separation**: The linker script physically separates privileged code/data into `.priv.*` sections that are unmapped at user privilege, providing a hardware-enforced security boundary.
6. **Pluggable scheduler policy**: The scheduler uses a virtual interface (`sched_policy`) so scheduling algorithms can be swapped without changing the core scheduler code.
7. **Compile-time log filtering**: Log calls below the configured level are completely eliminated by the compiler, ensuring zero runtime overhead for disabled log levels.
