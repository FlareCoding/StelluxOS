description: Gather context on the Stellux 3.0 kernel for a new agent session
mode: agent

---

You are onboarding yourself onto the Stellux 3.0 codebase. Your goal is to build a mental model of the project so you can assist effectively.

## Step 1: Read Project Philosophy & Rules

Read these files in order — they define how this project thinks:

1. `.cursor/rules/philosophy.md` — Core principles and problem-solving approach
2. `.cursor/rules/architecture.md` — Dual-architecture requirements (x86_64 + AArch64)
3. `.cursor/rules/style.md` — Naming and code style conventions
4. `.cursor/rules/low-level.md` — Low-level coding constraints
5. `.cursor/rules/error-handling.md` — Error handling approach
6. `.cursor/rules/build.md` — Build system details
7. `.cursor/rules/cpp-constraints.md` — C++ subset used (freestanding, no STL)

## Step 2: Understand the Directory Structure

Explore the top-level layout and key directories:

- `kernel/common/` — Architecture-independent interfaces and shared logic
- `kernel/arch/x86_64/` — x86_64 implementations
- `kernel/arch/aarch64/` — AArch64 implementations
- `kernel/boot/` — Boot protocol handling
- `kernel/mm/`, `kernel/trap/`, `kernel/sched/`, `kernel/syscall/` — Subsystems
- `boot/` — Bootloader-related code
- `Makefile`, `config.mk` — Build system entry points
- `scripts/` — Helper scripts

## Step 3: Read Key Source Files

Skim these to understand how the codebase actually works:

- A common header (e.g., one from `kernel/common/`) to see interface style
- The matching arch implementations in both `kernel/arch/x86_64/` and `kernel/arch/aarch64/`
- `kernel/Makefile` or `Makefile` to understand build flow
- Entry points: look for `start.S`, `main`, or `arch_init` in both architectures

## Step 4: Identify Current State

- What subsystems exist and how mature are they?
- What's the most recent area of development? (check git log)
- Are there any TODOs, FIXMEs, or incomplete areas?

## Output

Report back with a concise summary covering:

1. **Project Identity**: What Stellux 3.0 is and what makes it unique
2. **Architecture**: How the dual-arch abstraction works in practice
3. **Current State**: What subsystems exist and their maturity level
4. **Key Patterns**: Conventions a new contributor must follow
5. **Active Areas**: Where development is currently focused

Keep it brief and actionable — this is a working summary, not a report.