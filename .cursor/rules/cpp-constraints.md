---
description: Freestanding C++ constraints for kernel development
globs: ["kernel/**/*.cpp", "kernel/**/*.h"]
---

# C++ Constraints (Freestanding Environment)

The kernel runs in a freestanding C++20 environment without standard library support.

## Allowed Features

- C++20 standard language features
- Full template metaprogramming (keep compile times reasonable)
- `constexpr` / `consteval` for compile-time computation (preferred over macros)
- Namespaces, classes, structs, references, lambdas
- Move semantics, RAII patterns
- `static_assert` for compile-time checks
- Inline functions and variables

## Forbidden Features

- **Exceptions**: No `throw`, `try`, `catch` (compiled with `-fno-exceptions`)
- **RTTI**: No `dynamic_cast`, `typeid` (compiled with `-fno-rtti`)
- **Standard library**: No `<vector>`, `<string>`, `<iostream>`, `<memory>`, etc.
- **Global constructors**: No objects requiring runtime initialization (use `constexpr`)
- **Dynamic allocation**: No `new` / `delete` (until allocator is implemented)
- **Thread-local storage**: No `thread_local` keyword
- **Virtual inheritance**: Avoid due to complexity in freestanding environment

## Compile Flags Reference

The kernel is compiled with:
```
-std=c++20 -ffreestanding -fno-exceptions -fno-rtti -nostdinc++ -nostdlib
```
