---
description: Low-level code patterns for assembly, MMIO, and hardware interaction
globs: ["kernel/**/*.cpp", "kernel/**/*.h", "kernel/**/*.S"]
---

# Low-Level Code Rules

## Inline Assembly

Inline assembly must be isolated in small wrapper functions, never in business logic:

```cpp
// GOOD: Isolated wrapper function
inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline uint8_t inb(uint16_t port) {
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

// BAD: Inline asm in business logic
void serial_write(char c) {
    asm volatile("outb %0, %1" : : "a"(c), "Nd"(0x3F8));  // Don't do this!
}
```

## Volatile Usage

Use `volatile` only for:
- Memory-mapped I/O (MMIO) registers
- Shared memory with hardware
- Memory accessed by interrupt handlers

```cpp
// MMIO register access
volatile uint32_t* uart_data = reinterpret_cast<volatile uint32_t*>(0x09000000);
```

## Magic Numbers

All magic numbers must be named `constexpr` or `#define` constants:

```cpp
// GOOD
constexpr uint16_t COM1_PORT = 0x3F8;
constexpr uintptr_t PL011_BASE = 0x09000000;

// BAD
outb(0x3F8, data);  // What is 0x3F8?
```

## C++ Casts

Use C++ casts exclusively, never C-style casts:

- `static_cast<>` for safe conversions between related types
- `reinterpret_cast<>` for pointer/integer conversions (MMIO addresses)
- `const_cast<>` sparingly, only when interfacing with C APIs
- **Never** use C-style casts: `(type)value`

```cpp
// GOOD
auto* mmio = reinterpret_cast<volatile uint32_t*>(UART_BASE);
auto byte = static_cast<uint8_t>(value & 0xFF);

// BAD
auto* mmio = (volatile uint32_t*)UART_BASE;
auto byte = (uint8_t)(value & 0xFF);
```

## Memory Management

Until heap allocator is implemented:

- Static allocation only
- Fixed-size arrays with compile-time sizes
- Stack allocation for temporary data (kernel stack ~16KB)
- No external dependencies - implement everything from scratch
