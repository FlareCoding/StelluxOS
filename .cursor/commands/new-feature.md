description: Guide structured approach to implementing a new Stellux 3.0 feature
mode: agent

---

You are helping implement a new feature for Stellux 3.0, a dual-architecture (x86_64 + AArch64) operating system kernel.

## Required Reading

Before proceeding, read and internalize:
- `.cursor/rules/philosophy.md` - Core project philosophy and problem-solving approach

## Your Approach

Follow the Stellux 3.0 methodology for this feature:

### Phase 1: Understanding
First, ask clarifying questions to understand:
1. What is the feature? What problem does it solve?
2. What are the expected inputs/outputs/behaviors?
3. Are there any constraints or requirements I should know about?
4. What existing code does this interact with?

### Phase 2: Design (Semantics First)
Before writing any code, think through and present:

1. **Abstract Semantics**: What does this operation *mean* independent of architecture?

2. **Architecture Split**:
   - What belongs in `kernel/common/`? (interfaces, shared logic)
   - What belongs in `kernel/arch/x86_64/`? (x86-specific implementation)
   - What belongs in `kernel/arch/aarch64/`? (ARM-specific implementation)

3. **Interface Design**: Propose the common header interface with:
   - Function signatures
   - Types and constants
   - Error handling approach
   - Documentation comments

4. **Implementation Sketch**:
   - How will x86_64 implement this?
   - How will AArch64 implement this?
   - Do both feel natural, or is the abstraction fighting one of them?

5. **Edge Cases & Invariants**:
   - What can go wrong?
   - How will errors be handled?
   - What invariants must hold?

### Phase 3: Implementation
Only after the design is approved:
1. Create the common interface header
2. Implement for x86_64
3. Implement for AArch64
4. Verify both build and work

### Phase 4: Verification Checklist
- [ ] Builds without warnings: `make kernel ARCH=x86_64`
- [ ] Builds without warnings: `make kernel ARCH=aarch64`
- [ ] Works in QEMU x86_64: `make run ARCH=x86_64`
- [ ] Works in QEMU AArch64: `make run ARCH=aarch64`
- [ ] No `#ifdef __x86_64__` in common code
- [ ] All magic numbers are named constants
- [ ] Error cases handled explicitly
- [ ] Code follows snake_case style

## Important Reminders

- **Both architectures are required.** A feature is not done until it works on x86_64 AND AArch64.
- **Slow down and think.** A week on good abstractions saves months of refactoring.
- **Ask if unsure.** Don't guess at requirements or make assumptions.
- **No "fix it later" debt.** If something isn't right, fix it now.

---

What feature would you like to implement?
