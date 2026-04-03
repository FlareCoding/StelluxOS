---
description: Testing and debugging practices
globs: ["kernel/**"]
---

# Testing & Debugging

## Testing Approach

- Test in QEMU, verify behavior via serial output
- Use `make run-headless ARCH=<arch>` for SSH/CI environments
- Must pass on both x86_64 and AArch64 before feature is complete

## QEMU Commands

```bash
# With display
make run ARCH=x86_64

# Headless (serial to stdout)
make run-headless ARCH=x86_64

# Verbose build + run
make run ARCH=x86_64 V=1
```

## Log Levels

Implement compile-time configurable log levels:

| Level | When to Use | Enabled |
|-------|-------------|---------|
| `ERROR` | Unrecoverable issues | Always |
| `WARN` | Recoverable issues | Always |
| `INFO` | General information | Debug builds |
| `DEBUG` | Detailed debugging | Debug builds |
| `TRACE` | Very verbose | Explicit opt-in |

## Assertions

- Assertions enabled in debug builds (`DEBUG=1`)
- Compiled out in release builds (`RELEASE=1`)
- Use for invariant checking, not error handling

```cpp
// Assert for invariants
kernel_assert(ptr != nullptr);
kernel_assert(size > 0 && size <= MAX_SIZE);

// Don't use assert for expected error conditions
// Use error codes instead
```

## Type Aliases for Clarity

Use typedef/using aliases for semantic clarity:

```cpp
using phys_addr = uint64_t;
using virt_addr = uint64_t;
using port_t = uint16_t;
using error_t = int32_t;
```

This makes code self-documenting and enables future type safety improvements.
