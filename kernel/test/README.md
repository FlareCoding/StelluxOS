# Stellux Kernel Unit Tests

This directory contains the in-kernel unit testing framework and suites for Stellux 3.0.

## Design goals

- Freestanding-friendly (no STL, no exceptions, no runtime constructors)
- Deterministic execution and machine-readable results
- Easy test registration and easy test authoring
- Dual-architecture compatible (x86_64 + aarch64)

## Layout

- `framework/`
  - `test_framework.h` — registration + assertion macros
  - `test_runner.h/.cpp` — execution engine
  - `test_registry.h` — descriptor types and phases
  - `test_utils.h` — deterministic PRNG helpers for tests
- `common/`
  - core + memory-layer suites

## Running tests

From the repository root:

```bash
# Run all suites for one architecture
make test ARCH=x86_64
make test ARCH=aarch64

# Run both architectures
make test-all

# Filter by suite prefix
make test ARCH=x86_64 UNIT_TEST_FILTER=core_
make test ARCH=x86_64 UNIT_TEST_FILTER=mm_

# Filter one case
make test ARCH=x86_64 UNIT_TEST_FILTER=core_string.strlen_handles_basic_inputs
```

Useful options:

- `UNIT_TEST_FAIL_FAST=1`
- `UNIT_TEST_REPEAT=<n>`
- `UNIT_TEST_SEED=<u64>`
- `UNIT_TEST_TIMEOUT=<seconds>`

## Writing tests

### 1) Declare a suite

```cpp
STLX_TEST_SUITE(core_string, test::phase::early);
```

or with per-case hooks:

```cpp
STLX_TEST_SUITE_EX(my_suite, test::phase::post_mm, before_each, after_each);
```

### 2) Add cases

```cpp
STLX_TEST(core_string, strlen_handles_basic_inputs) {
    STLX_ASSERT_EQ(ctx, string::strlen("abc"), static_cast<size_t>(3));
}
```

### 3) Assertions

- `STLX_EXPECT_*` records failure and continues
- `STLX_ASSERT_*` records failure and aborts current case
- `STLX_SKIP(ctx, "reason")` marks case skipped

Common checks:

- `STLX_EXPECT_TRUE/FALSE`
- `STLX_EXPECT_EQ/NE`
- `STLX_EXPECT_NULL/NOT_NULL`
- `STLX_EXPECT_STREQ`

## Phase guidance

- `test::phase::early`:
  - tests that do not require PMM/paging/KVA/VMM initialization
- `test::phase::post_mm`:
  - tests that require memory subsystems to be active

## CI output markers

The runner emits:

- `[TEST_SUMMARY] ...`
- `[TEST_RESULT] PASS`
- `[TEST_RESULT] FAIL`

The host script (`scripts/run-unit-tests.py`) parses these markers and returns appropriate exit codes for CI.
