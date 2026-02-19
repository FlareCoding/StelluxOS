# AGENTS.md

## Onboarding

Before starting any task, new agents should perform the equivalent of the `/gather-context` command (see `.cursor/commands/gather-context.md`). In short:

1. **Read project rules** (in order): `.cursor/rules/philosophy.md`, `architecture.md`, `style.md`, `low-level.md`, `error-handling.md`, `build.md`, `cpp-constraints.md`
2. **Explore directory structure**: `kernel/common/` (arch-independent interfaces), `kernel/arch/{x86_64,aarch64}/` (implementations), `kernel/boot/`, `kernel/mm/`, `kernel/trap/`, `kernel/sched/`, `kernel/syscall/`
3. **Read key source files**: a common header + both arch implementations, `start.S` and `arch_init` for both architectures, `Makefile` and `kernel/Makefile`
4. **Check current state**: `git log --oneline -20` to see recent development focus, and scan for TODOs/FIXMEs

This context is essential — the project has strict dual-architecture requirements, a freestanding C++20 subset (no STL), and specific coding conventions that are easy to violate without reading the rules first.

## Cloud-specific instructions

### Overview

Stellux 3.0 is a bare-metal hobby OS kernel written in freestanding C++20 and assembly, targeting **x86_64** and **AArch64**. There is no web server, database, or container infrastructure. The "application" is the kernel itself, tested by booting in QEMU and inspecting serial output.

### Prerequisites (system packages)

System dependencies are installed via `make deps` (wraps `sudo apt install`). The Limine UEFI bootloader binaries are fetched via `make limine` (downloads from GitHub). Both are one-time setup steps already handled by the update script.

### Build & Run commands

See `Makefile` help (`make help`) and `.cursor/rules/build.md` for the full command reference. Key commands:

- **Build**: `make kernel ARCH=x86_64` / `make kernel ARCH=aarch64`
- **Disk image**: `make image ARCH=x86_64` / `make image ARCH=aarch64`
- **Run with display**: `make run ARCH=x86_64`
- **Run headless** (for cloud/CI): `make run-headless ARCH=x86_64`
- **Lint**: `bash scripts/dynpriv-lint.sh`
- **Toolchain check**: `make toolchain-check`

### Gotchas for cloud environments

- **Headless mode required**: Use `make run-headless ARCH=<arch>` since there is no display server. QEMU uses `-nographic` and serial output goes to stdout.
- **OVMF_VARS.fd is mutable**: The x86_64 QEMU target requires a writable copy of the OVMF VARS file at `build/OVMF_VARS.fd`. This is auto-created by `make run*` targets, but if `build/` was cleaned you need to rebuild via `make image ARCH=x86_64` first.
- **AArch64 QEMU is slower**: AArch64 runs under TCG emulation (no KVM for cross-arch), so QEMU boot takes ~20-40s. Use `timeout 45` when scripting.
- **No automated test suite**: There is no unit test framework. Verification is done by booting the kernel in QEMU and checking serial output for `[INFO]  Stellux 3.0 booting...` through `Initialization complete! Halting...`.
- **Both architectures must build and pass**: A feature is not complete until both `ARCH=x86_64` and `ARCH=aarch64` build without errors and boot successfully.
- **Linter**: `scripts/dynpriv-lint.sh` checks dynamic privilege annotation consistency (header vs source `__PRIVILEGED_CODE` markers and docstrings). Run it before committing changes to kernel code.
