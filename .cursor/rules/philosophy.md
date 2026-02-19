---
description: Core project philosophy and problem-solving approach for Stellux 3.0
globs: ["**/*"]
---

# Stellux 3.0 Philosophy

## Project Context

Stellux 3.0 is a **ground-up rewrite** of a previous hobby OS (Stellux 1.x/2.x) that grew to include XHCI drivers, SMP, userspace, and libc integration - but suffered from foundational brittleness and x86-only assumptions baked into the core. This rewrite applies hard-won lessons to build a robust, scalable foundation.

**This is not a learning project.** The developer has real OS experience. The goal is to do things *right* this time.

## Core Principles

### 1. Dual Architecture as a Forcing Function

Both x86_64 and AArch64 are **first-class citizens from day one**. This is not about "supporting ARM" - it's about using architecture differences to *drive* proper abstractions.

- If a feature works cleanly on both architectures, the abstraction is probably right
- If it's hard to make work on both, that's a signal the abstraction is wrong
- Architecture differences expose hidden assumptions and force better design

**Target platforms:** ARM servers, Raspberry Pi, QEMU, x86 real hardware

### 2. Slow is Smooth, Smooth is Fast

Every feature must be:
- **Complete on both architectures** before being considered done
- **Well-thought-out** in semantics and layering
- **Robust enough to build upon** - no "fix it later" technical debt

Take time to design properly. A week spent on correct abstractions saves months of refactoring.

### 3. Robustness Over Features

The previous Stellux had many features but cracked under its own weight. Stellux 3.0 prioritizes:
- Correctness over speed-to-feature
- Clean abstractions over clever hacks
- Explicit error handling over silent failures
- Documented invariants over tribal knowledge

### 4. Monolithic with Clean Internals (Current Direction)

Starting as a monolithic kernel with strong internal abstractions. Not microkernel IPC overhead, but not spaghetti either. Architecture may evolve (microkernel, exokernel, hybrid) but the foundation must support any direction.

## How to Approach Problems

When implementing a new feature or solving a problem, follow this thinking process:

### Step 1: Semantics First

Ask: **What does this operation *mean* abstractly?**

- Think "map a page" not "write to CR3"
- Think "send interrupt to CPU" not "write to LAPIC/GIC"
- Define the operation independent of how any architecture implements it

### Step 2: Architecture Split

Ask: **What's common? What's arch-specific? Where's the boundary?**

- Common: The abstract operation, data structures, algorithms, policies
- Arch-specific: The mechanism, registers, instructions, hardware details
- The boundary should be a clean interface that both architectures implement

### Step 3: Interface Design

Ask: **What should the common header declare?**

Design the interface in `kernel/common/` that:
- Expresses the abstract semantics
- Hides architecture details completely
- Documents the contract implementations must fulfill
- Uses proper error handling (return codes, not exceptions)

### Step 4: Implementation Sketch

For both architectures, consider:
- **x86_64**: How does this work? What registers/instructions/mechanisms?
- **AArch64**: How does this work? Same questions.
- **Comparison**: Are the abstractions holding? Do both implementations feel natural, or is one fighting the interface?

If one architecture feels hacky, revisit the abstraction.

### Step 5: Edge Cases and Invariants

- What can go wrong? (hardware failures, invalid inputs, resource exhaustion)
- How do we handle errors? (return codes, panic for unrecoverable)
- What invariants must always hold? (document these explicitly)
- What are the concurrency considerations? (even before SMP, think ahead)

## Red Flags to Watch For

- **`#ifdef __x86_64__` in common code**: Use abstractions, not conditionals
- **"This is x86-specific, we'll port later"**: No. Both architectures, always.
- **Magic numbers without names**: Every constant needs a `constexpr` or `#define`
- **Inline assembly in business logic**: Isolate in small wrapper functions
- **"It works, ship it"**: Does it work on *both* architectures? Is it *robust*?
- **Silent failures**: Every error must be handled or explicitly propagated

## Example: Thinking Through a Feature

**Feature:** Serial port output for debugging

**Step 1 - Semantics:** "Write a character/string to the debug console"

**Step 2 - Split:**
- Common: `debug_putc(char c)`, `debug_puts(const char* s)`, initialization
- Arch-specific: x86 uses port I/O (0x3F8), AArch64 uses MMIO to PL011 UART

**Step 3 - Interface:**
```cpp
// kernel/common/debug.h
namespace kernel::debug {
    void init();
    void putc(char c);
    void puts(const char* s);
}
```

**Step 4 - Implementation:**
- x86: `outb(0x3F8, c)` with status polling
- AArch64: Write to PL011 data register with MMIO, check status register
- Both feel natural - abstraction is good

**Step 5 - Edge cases:**
- What if serial isn't available? (init returns error, putc becomes no-op)
- Buffer full? (poll/wait or drop?)
- Concurrency? (spinlock when SMP comes)

## Remember

The goal is not to ship features fast. The goal is to build a foundation that can support a real operating system - one that doesn't crack under its own weight like the previous version did.

**When in doubt, slow down and think.**
