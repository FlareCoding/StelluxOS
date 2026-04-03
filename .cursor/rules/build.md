---
description: Build system usage and requirements
globs: ["Makefile", "**/*.mk", "kernel/**"]
---

# Build System

## Required Parameters

Always specify `ARCH` for builds:

```bash
make kernel ARCH=x86_64
make kernel ARCH=aarch64
```

## Common Commands

```bash
# Setup (run once)
make deps                      # Install system packages
make limine                    # Download bootloader

# Configuration
make defconfig ARCH=x86_64     # Generate config.mk

# Build
make kernel ARCH=x86_64        # Build kernel
make image ARCH=x86_64         # Build + create disk image

# Run
make run ARCH=x86_64           # Run with display
make run-headless ARCH=x86_64  # Run headless (for SSH)

# Debug
make kernel ARCH=x86_64 V=1    # Verbose output
make kernel ARCH=x86_64 DEBUG=1    # Debug build (default)
make kernel ARCH=x86_64 RELEASE=1  # Release build
```

## Build Modes

| Mode | Flags | Use Case |
|------|-------|----------|
| Debug (default) | `-O0 -g -DDEBUG` | Development, debugging |
| Release | `-O2 -DNDEBUG` | Performance testing, deployment |

## Testing Checklist

Before considering a feature complete:

1. `make kernel ARCH=x86_64` - Builds without errors/warnings
2. `make kernel ARCH=aarch64` - Builds without errors/warnings
3. `make run ARCH=x86_64` - Runs correctly in QEMU
4. `make run ARCH=aarch64` - Runs correctly in QEMU
5. `make kernel ARCH=x86_64 RELEASE=1` - Release build works

## Troubleshooting

- Use `make V=1` to see full compiler commands
- Use `make toolchain-check` to verify tools are installed
- Use `make clean` before rebuilding after Makefile changes
