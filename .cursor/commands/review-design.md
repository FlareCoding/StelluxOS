description: Review an existing abstraction or design decision in Stellux 3.0
mode: agent

---

You are reviewing an existing design or abstraction in Stellux 3.0 to verify it follows project principles.

## Required Reading

First, read:
- `.cursor/rules/philosophy.md` - Core project philosophy

## Review Checklist

Analyze the code/design against these criteria:

### 1. Architecture Independence
- Is common code truly architecture-independent?
- Are there any `#ifdef __x86_64__` or `#ifdef __aarch64__` in common files?
- Does the abstraction hide arch details, or do they leak through?

### 2. Abstraction Quality
- Does the interface express *what* not *how*?
- Would a new architecture (e.g., RISC-V) fit naturally?
- Are both x86_64 and AArch64 implementations clean, or is one fighting the interface?

### 3. Error Handling
- Are all error cases handled explicitly?
- Do functions return proper error codes?
- Are there any silent failures?

### 4. Style & Documentation
- snake_case for functions, types, variables?
- SCREAMING_SNAKE_CASE for constants?
- Javadoc comments on public APIs?
- Comments explain WHY, not WHAT?

### 5. Low-Level Correctness
- Inline assembly isolated in wrapper functions?
- All magic numbers named?
- C++ casts (not C-style)?
- Volatile only where needed (MMIO, hardware)?

### 6. Robustness
- What happens if this fails?
- Are invariants documented?
- Is this building on solid foundations or shaky assumptions?

## Output Format

Provide:
1. **Summary**: Overall assessment (good / needs work / problematic)
2. **Strengths**: What's done well
3. **Issues**: Specific problems found with file:line references
4. **Recommendations**: Concrete suggestions for improvement

---

What would you like me to review?
