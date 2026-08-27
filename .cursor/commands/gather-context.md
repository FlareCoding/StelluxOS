description: Gather context on the StelluxOS kernel for a new agent session
mode: agent

---

You are onboarding yourself onto the StelluxOS codebase. Your goal is to build a mental model of the project so you can assist effectively.

## Step 1: Read the project rules and skills

These define how the project thinks and what it expects:

1. `.cursor/rules/philosophy.mdc`: core principles and the design process
2. `.cursor/rules/arch-layout.mdc`: the include overlay and where code belongs
3. `.cursor/rules/dynpriv.mdc`: what may be privileged and how elevation is bracketed
4. `.cursor/rules/style.mdc`: naming, comments, stanza formatting, structure
5. `.cursor/rules/stellux-conventions.mdc`: commits, review workflow, verification gates
6. `.cursor/skills/stellux-dynpriv/SKILL.md`: the dynamic privilege model in depth
7. `README.md`: the research motivation and supported platforms

## Step 2: Understand the directory structure

Explore the layout, ignoring build artifacts:

- `kernel/<subsystem>/`, one directory per subsystem, namespace matches the directory
- `kernel/common/`, freestanding types, logging, containers, string utilities
- `kernel/arch/{x86_64,aarch64}/`, per-architecture implementations that mirror subsystem names
- `kernel/tests/`, unit tests grouped by theme, built only with STLX_UNIT_TESTS_ENABLED
- `userland/lib/`, `userland/apps/`, musl and libc++ based userland
- `Makefile`, `config.mk`, `kernel/Makefile`, build entry points
- `scripts/`, helper scripts including dynpriv-lint.sh and priv-footprint.sh

## Step 3: Read key source files

- `kernel/boot/boot.cpp` for the init sequence and how subsystems come up
- `kernel/dynpriv/dynpriv.h` plus one arch implementation, since dynamic privilege is the project's thesis
- A common interface and both arch implementations of it, for example `kernel/clock/clock.h`
- Entry points: `start.S` and `arch_init.cpp` in both architectures

## Step 4: Identify current state

- Which subsystems exist and how mature are they
- Where recent development has focused (check git log)
- Any incomplete areas

## Output

Report back a concise summary covering:

1. **Project identity**: what StelluxOS is and what makes it unique
2. **Architecture**: how the dual-arch abstraction works in practice
3. **Current state**: subsystems and maturity
4. **Key patterns**: conventions a contributor must follow
5. **Active areas**: where development is focused

Keep it brief and actionable, a working summary rather than a report.
