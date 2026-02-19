---
description: Code style and naming conventions
globs: ["kernel/**/*.cpp", "kernel/**/*.h", "kernel/**/*.h"]
---

# Code Style & Naming Conventions

## Naming Rules

- `snake_case` for everything: functions, variables, types, structs, classes, namespaces
- `SCREAMING_SNAKE_CASE` for constants, `constexpr` values, and `#define` macros
- File names: `snake_case.h`, `snake_case.cpp`, `snake_case.S`

## Examples

```cpp
// Types (snake_case)
struct frame_buffer { ... };
class memory_manager { ... };
using phys_addr = uint64_t;

// Functions (snake_case)
void serial_init();
int32_t map_page(phys_addr pa, virt_addr va);

// Constants (SCREAMING_SNAKE_CASE)
constexpr uint16_t COM1_PORT = 0x3F8;
constexpr size_t KERNEL_STACK_SIZE = 16384;
#define PAGE_SIZE 4096

// Namespaces (snake_case)
namespace kernel::serial { ... }
namespace kernel::mm { ... }
```

## File Organization

- `.h` / `.cpp` pairs - headers declare interfaces, source files implement
- One class/module per file (exceptions for small related utilities)
- Namespace per subsystem: `kernel::serial`, `kernel::mm`, `kernel::console`

## Include Guards

Use traditional `#ifndef` / `#define` / `#endif` guards (not `#pragma once`):

```cpp
#ifndef STELLUX_KERNEL_SERIAL_H
#define STELLUX_KERNEL_SERIAL_H

// ... content ...

#endif // STELLUX_KERNEL_SERIAL_H
```

Guard naming: `STELLUX_PATH_TO_FILE_H`

## Brace Style

Opening brace on the same line as the declaration (K&R style):

```cpp
// GOOD
inline void foo() {
    // ...
}

if (condition) {
    // ...
}

// BAD
inline void foo()
{
    // ...
}
```

## Comments

- Keep comments concise - avoid verbose file headers and section separators
- Do NOT use decorative separators like `// ============` or `// ----------`
- Function comments should be brief (2-3 lines max for simple functions)
- Save detailed documentation for complex algorithms or non-obvious behavior
- **Single space before inline comments** - use one space between code and `//`, not two

```cpp
// GOOD - single space before inline comment
constexpr uint16_t COM1_PORT = 0x3F8; // Serial port base
} // namespace serial
#endif // STELLUX_KERNEL_SERIAL_H

// BAD - double space before inline comment
constexpr uint16_t COM1_PORT = 0x3F8;  // Serial port base
}  // namespace serial
#endif  // STELLUX_KERNEL_SERIAL_H
```

```cpp
// GOOD - brief and informative
/**
 * SMP read barrier - ensures load-load ordering between CPUs.
 * On x86_64: Compiler barrier only (TSO guarantees load-load ordering).
 */
inline void smp_read() {
    asm volatile("" ::: "memory");
}

// BAD - overly verbose, decorative separators
// ============================================================================
// SMP Barriers (CPU-to-CPU Synchronization)
// ============================================================================

/**
 * @file barrier.h
 * @brief Memory barrier primitives for x86_64.
 *
 * x86_64 Memory Model (Total Store Order - TSO):
 * ... 30 lines of explanation ...
 */
```

## Other Rules

- Minimize global state; when needed, keep in dedicated files with clear ownership
- Forward declare when possible to reduce header dependencies
- Use `using` aliases for type clarity: `using phys_addr = uint64_t;`
