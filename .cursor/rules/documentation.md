---
description: Documentation and comment standards
globs: ["kernel/**/*.cpp", "kernel/**/*.h", "kernel/**/*.h"]
---

# Documentation Standards

## Function Documentation

Use Javadoc-style comments for public APIs:

```cpp
/**
 * Initialize the serial port for debugging output.
 *
 * @param port The I/O port address (e.g., COM1_PORT for x86_64)
 * @param baud The baud rate (default: 115200)
 * @return 0 on success, negative error code on failure
 *         - E_INVAL: Invalid port or baud rate
 *         - E_BUSY: Port already in use
 */
int32_t serial_init(uint16_t port, uint32_t baud = 115200);
```

## Comment Philosophy

- Comments explain **WHY**, not **WHAT** - code should be self-documenting
- If you need to explain what code does, consider refactoring for clarity
- Document non-obvious decisions, workarounds, and hardware quirks

```cpp
// GOOD: Explains why
// x86_64 requires red zone to be disabled in kernel mode
// because interrupts can clobber the 128-byte area below RSP

// BAD: Explains what (obvious from code)
// Initialize the variable to zero
int count = 0;
```

## TODO Comments

Use `TODO(name): description` format for incomplete work:

```cpp
// TODO(flare): Implement timeout handling for UART transmit
// TODO(flare): Add support for higher baud rates
```

All TODOs must be addressed before release.

## Architecture Documentation

Document architecture differences when relevant:

```cpp
/**
 * Write a byte to the serial port.
 *
 * Architecture notes:
 * - x86_64: Uses port I/O (outb instruction)
 * - AArch64: Uses MMIO to PL011 UART
 */
void serial_putc(char c);
```
