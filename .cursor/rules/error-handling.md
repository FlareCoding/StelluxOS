---
description: Error handling patterns for kernel code
globs: ["kernel/**/*.cpp", "kernel/**/*.h"]
---

# Error Handling

Without exceptions, the kernel uses error codes and panic for error handling.

## Error Codes (Recoverable Errors)

Return error codes for recoverable errors:

- Return `0` for success
- Return negative values for errors (e.g., `-1`, `-EINVAL`, `-ENOMEM`)
- Define error codes as named constants in a central header
- For functions that return data, use out parameters or return `nullptr` on failure

```cpp
// Example error codes
constexpr int32_t E_SUCCESS = 0;
constexpr int32_t E_INVAL   = -1;   // Invalid argument
constexpr int32_t E_NOMEM   = -2;   // Out of memory
constexpr int32_t E_BUSY    = -3;   // Resource busy
constexpr int32_t E_NOENT   = -4;   // Not found

// Function returning error code
int32_t map_page(phys_addr pa, virt_addr va);

// Function with out parameter
int32_t read_register(uint32_t reg, uint32_t* out_value);
```

## Panic (Unrecoverable Errors)

For unrecoverable errors, panic:

1. Print diagnostic message to serial
2. Display debug info (file, line, function)
3. Optionally dump registers
4. Halt the CPU

```cpp
// Panic should output:
// PANIC: <message>
// Location: <file>:<line> in <function>
// [optional register dump]
// Then halt
```

## Rules

- Always check pointers for null before dereferencing
- No silent failures - every error must be handled or explicitly propagated
- Document all possible error codes in function comments
- Prefer early return on error (guard clauses)

```cpp
int32_t do_something(void* ptr) {
    if (ptr == nullptr) {
        return E_INVAL;
    }
    // ... rest of function
    return E_SUCCESS;
}
```
