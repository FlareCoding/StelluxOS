description: Debug an issue that appears on one or both architectures
mode: agent

---

You are debugging an issue in Stellux 3.0. The kernel targets both x86_64 and AArch64.

## Required Reading

First, read:
- `.cursor/rules/philosophy.md` - Project context and approach

## Diagnostic Process

### Step 1: Characterize the Problem
- Which architecture(s) exhibit the issue? (x86_64 / AArch64 / both)
- What is the expected behavior?
- What is the actual behavior?
- Is it a build error, runtime crash, or incorrect behavior?

### Step 2: Reproduce
- What exact commands reproduce the issue?
- Is it consistent or intermittent?
- What was the last known working state?

### Step 3: Narrow Down

**If it affects only ONE architecture:**
- The bug is likely in `kernel/arch/{arch}/` code
- Or in how that arch implements a common interface
- Check recent changes to arch-specific files

**If it affects BOTH architectures:**
- The bug is likely in `kernel/common/` or `kernel/boot/`
- Or it's a fundamental design issue
- Check the abstraction layer

### Step 4: Investigate
- Read relevant source files
- Check serial output (add debug prints if needed)
- Examine the build commands (`make V=1`)
- Check linker script if it's an address/symbol issue

### Step 5: Fix
- Identify root cause (not just symptoms)
- Fix in the appropriate layer (common vs arch-specific)
- Verify fix works on BOTH architectures
- Ensure no regressions

## Common Pitfalls

- **Assuming it's arch-specific when it's not**: Always test both
- **Fixing symptoms not causes**: Understand WHY before patching
- **Breaking the other arch**: Always run both `make kernel ARCH=x86_64` and `make kernel ARCH=aarch64`

## Build/Run Commands

```bash
# Verbose build (see actual compiler commands)
make kernel ARCH=x86_64 V=1
make kernel ARCH=aarch64 V=1

# Run with serial output
make run ARCH=x86_64
make run ARCH=aarch64

# Headless (serial to terminal)
make run-headless ARCH=x86_64
make run-headless ARCH=aarch64

# Clean rebuild
make clean && make kernel ARCH=x86_64
```

---

Describe the issue you're experiencing:
