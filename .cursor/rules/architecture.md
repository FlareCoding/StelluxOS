---
description: Multi-architecture requirements for kernel code
globs: ["kernel/**"]
---

# Architecture Requirements

This kernel targets both x86_64 and AArch64 architectures.

## Core Rules

- Every feature MUST work on both x86_64 and AArch64 before being considered complete
- Architecture-specific code goes in `kernel/arch/{x86_64,aarch64}/`
- Common headers in `kernel/common/` define interfaces; arch directories provide implementations
- Arch-specific features must be behind an abstraction layer

## Implementation Pattern

When implementing a new feature:

1. Create the common interface first in `kernel/common/`
2. Implement for x86_64 in `kernel/arch/x86_64/`
3. Implement for AArch64 in `kernel/arch/aarch64/`
4. Test on both architectures before merging

## Prohibited

- Never use `#ifdef __x86_64__` or `#ifdef __aarch64__` in common code
- Use the abstraction layer instead of conditional compilation in shared files
- Architecture detection is only allowed in arch-specific directories

## File Organization

```
kernel/
├── common/           # Architecture-independent code (interfaces)
├── arch/
│   ├── x86_64/       # x86_64-specific implementations
│   └── aarch64/      # AArch64-specific implementations
```
