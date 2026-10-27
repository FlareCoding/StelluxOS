#!/usr/bin/env python3
"""Tests for `style_lint.py`, run with `python3 scripts/style_lint_test.py`."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

import style_lint

SCRIPT = Path(__file__).resolve().with_name("style_lint.py")
GIT_IDENTITY = ("-c", "user.name=Style Lint", "-c", "user.email=style-lint@example.com", "-c", "commit.gpgsign=false")
E_ACUTE = "U+00E9 (latin small letter e with acute)"
I_DIAERESIS = "U+00EF (latin small letter i with diaeresis)"

# Real kernel and userland comments that look like violations but are not, Usage examples included
REAL_COMMENTS = """\
/**
 * @brief Copy from a user buffer to a kernel buffer. A user page that is
 * missing or unreadable at the moment of the copy reports ERR_FAULT.
 * @return The earliest deadline still in the future, NO_DEADLINE when none.
 * @brief Start accounting the calling CPU's time at the moment of the call.
 */
enum {
    /* Deliver one STLXWIN_EVT_FRAME after this commit is presented.
     * Request again on each commit for continuous animation. */
    STLXWIN_COMMIT_WANT_FRAME = 1u << 0,
};
    // Fix up if we removed a black node
/*
 * Usage:
 *   struct my_key_ops {
 *       using key_type = uint64_t;
 *       static key_type key_of(const my_item& e) { return e.id; }
 *       static bool equal(const key_type& a, const key_type& b) { return a == b; }
 *   };
 *
 *   hashmap::bucket buckets[64] = {};
 *   table.init(buckets, 64);
 *   list::head<my_item, &my_item::link> my_list;
 *   my_item* front = my_list.pop_front();
 *   struct my_compare {
 *       bool operator()(const my_item& a, const my_item& b) const {
 *           return a.key < b.key;
 *       }
 *   };
 *   rbt::tree<my_item, &my_item::tree_link, my_compare> my_tree;
 */
"""


def lint(text: str, path: str = "example.cpp") -> list[tuple[bool, str]]:
    """Returns each finding in `text`, every line counted as added, as whether it is an error and its problem."""
    findings = style_lint.check_file(path, text, None)
    return [(finding.rule.is_error, finding.message.split(",")[0]) for finding in findings]


def git(*arguments: str) -> str:
    return subprocess.run(["git", *GIT_IDENTITY, *arguments], check=True, capture_output=True, text=True).stdout


def write(path: str, text: str) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(text)


class ScanTest(unittest.TestCase):
    def test_each_file_type_reads_its_own_comment_syntax(self):
        text = "x /* \u00e9 */ # \u00ef"
        c_comments = [(True, f"{I_DIAERESIS} outside a comment"), (True, f"{E_ACUTE} in a comment")]
        hash_comments = [(True, f"{E_ACUTE} outside a comment"), (True, f"{I_DIAERESIS} in a comment")]
        cases = [(path, c_comments) for path in ("a.c", "a.cpp", "a.h", "a.hpp", "a.S", "a.ld", "a.inc")]
        cases += [(path, hash_comments) for path in ("a.sh", "a.mk", "Makefile")]

        for path, expected in cases:
            with self.subTest(path):
                self.assertEqual(lint(text, path), expected)

    def test_scanning_splits_each_line_into_its_parts(self):
        c_lines = style_lint.scan_file("example.cpp", "puts(\"a // {\", '}'); // one\n/* two\n * three\n   four */")
        shell_lines = style_lint.scan_file("example.sh", "echo \"a # b\" $# ${#c} # one\necho two")

        self.assertEqual(c_lines[0], (1, "puts(\"a // {\", '}'); // one", " one", "\"a // {\" '}'", "puts(\"\", ''); "))
        self.assertEqual([line.comment for line in c_lines[1:]], ["two", " three", "four "])

        shell_parts = [(" one", "\"a # b\"", "echo \"\" $# ${#c} "), (None, "", "echo two")]
        self.assertEqual([line[2:] for line in shell_lines], shell_parts)


class LineCheckTest(unittest.TestCase):
    def test_each_rule_reports_its_violation(self):
        cases = (
            ("int caf\u00e9 = 0;", True, f"{E_ACUTE} outside a comment"),
            ("puts(\"caf\u00e9\");", True, f"{E_ACUTE} outside a comment"),
            ("// caf\u00e9", True, f"{E_ACUTE} in a comment"),
            ("// names `caf\u00e9`", True, f"{E_ACUTE} in a comment"),
            ("// one \u2014 two", True, "U+2014 (em dash) is a typographic dash"),
            ("// names `a\u2013b`", True, "U+2013 (en dash) is a typographic dash"),
            ("// one -- two", True, "dash as punctuation"),
            ("// one - two", True, "dash as punctuation"),
            ("// ends on a dash --", True, "dash as punctuation"),
            ("// the ring (full) - drops frames", True, "dash as punctuation"),
            ("// drops frames - (see the ring)", True, "dash as punctuation"),
            ("// `transmit` - sends the frame", True, "dash as punctuation"),
            ("// one; two", True, "semicolon in a comment"),
            ("// `transmit` copies the frame; the caller keeps it", True, "semicolon in a comment"),
            ("// a robust parser", True, "'robust' is a banned word"),
            ("// simply returns", True, "'simply' is a banned word"),
            ("// this PR adds a cache", True, "'this PR adds' dates the comment"),
            ("// cleaned up in this patch", True, "'in this patch' dates the comment"),
            ("// fixed since this patch", True, "'since this patch' dates the comment"),
            ("// moved by the refactor", True, "'the refactor' dates the comment"),
            ("// stubbed for now", True, "'for now' dates the comment"),
            ("// sized as discussed", True, "'as discussed' dates the comment"),
            ("// *****", True, "decorative separator"),
            ("//////", True, "decorative separator"),
            ("/* --- API --- */", True, "decorative separator"),
            ("// now returns the count", False, "'now returns' may date the comment"),
            ("// the old implementation spun", False, "'old implementation' may date the comment"),
            ("// unsupported at the moment", False, "'at the moment' may date the comment"),
            ("// In the future the cache moves", False, "'In the future' may date the comment"),
            ("// We changed the order", False, "'We changed' may date the comment"),
            ("// this commit adds a flag", False, "'this commit adds' may date the comment"),
            ("// stubbed until the allocator lands", False, "'until the allocator lands' may date the comment"),
            ("// utilize the cache", False, "'utilize' is filler"),
            ("// TODO drop the retry", False, "'TODO' marks unfinished work"),
        )

        for text, is_error, problem in cases:
            with self.subTest(text):
                self.assertEqual(lint(text), [(is_error, problem)])

    def test_exempt_text_reports_nothing(self):
        cases = (
            "// `make -- -j1`, `a; b`, and `robust_mode` name code",
            """
                /*
                 * Usage:
                 *   int x = 1; int y = 2;
                 *   elapsed = now - start;
                 *   -- --verbose
                 *    -------
                 *   | box |
                 *    -------
                 */
            """,
            "// +---+-----+\n// | a | b c |\n// ----+------",
            "// as discussed above\n// waits until the queue drains and the frame lands",
            "// holds count - 16 slots, a simple ring",
            "//   int x; int y;",
            "/**\n * @return OK when\n *   \n *   int x; int y;\n */",
        )

        for text in cases:
            with self.subTest(text):
                self.assertEqual(lint(text), [])

    def test_wrapped_tags_and_list_items_stay_prose(self):
        text = """
            /**
             * @return OK on success, ERR_BUSY when
             *         no slot is free; the caller retries.
             *         Each retry waits; it never spins.
             * - an item that wraps
             *   onto a second line; still prose
             * 1. a numbered item that
             *    wraps; also prose
             * Usage:
             *   int x = 1; int y = 2;
             * @code
             *   int z = 3; int w = 4;
             * @endcode
             */
        """
        numbers = [finding.number for finding in style_lint.check_file("example.cpp", text, None)]
        self.assertEqual(numbers, [4, 5, 7, 9])

    def test_a_dash_does_not_hide_another_non_ascii_character(self):
        findings = lint("// one \u2014 two \u2192 three")
        expected = [(True, "U+2192 (rightwards arrow) in a comment"), (True, "U+2014 (em dash) is a typographic dash")]
        self.assertEqual(findings, expected)

    def test_real_comments_report_nothing(self):
        self.assertEqual(lint(REAL_COMMENTS), [])


class StanzaTest(unittest.TestCase):
    def test_a_stanza_of_more_than_six_statements_warns(self):
        six = "void f() {\n" + "    a();\n" * 6 + "\n    b();\n}\n"
        seven = "void f() {\n" + "    a();\n" * 7 + "}\n"

        self.assertEqual(lint(six), [])
        self.assertEqual(lint(seven), [(False, "the stanza from line 2 holds 7 statements")])

    def test_stanzas_are_read_in_c_and_cpp_files_to_their_last_line(self):
        seven_without_final_newline = "void f() {\n" + "    a();\n" * 7 + "}"
        for path in ("example.c", "example.cpp", "example.h", "example.hpp"):
            with self.subTest(path):
                expected = [(False, "the stanza from line 2 holds 7 statements")]
                self.assertEqual(lint(seven_without_final_newline, path), expected)

        self.assertEqual(lint(seven_without_final_newline, "example.S"), [])

    def test_a_statement_counts_once_whatever_it_spans(self):
        text = """
            int32_t f(int n) {
                // a comment and a directive count as no statement
            #if defined(X) && X == 1
                int32_t rc = call(
                    n,
                    [&](int x) { a(x); b(x); }
                );
            #endif
                for (int i = 0; i < n; i++) { a(i); }
                do { n--; } while (n > 0);
                config c = {.lanes = 4, .rate = {1, 2}};
                auto g = [&]() { return n; };
                c.apply(g);
                if (n) {
                    a(n);
                } else if (n > 1) {
                    b(n);
                } else {
                    return {rc, n};
                }
            }
        """
        self.assertEqual(lint(text), [(False, "the stanza from line 3 holds 7 statements")])

    def test_blank_lines_and_labels_end_a_stanza(self):
        text = """
            void f(int n) {
                switch (n) {
                case 1:
                    a(); a(); a(); a();
                    break;
                default:
                    b(); b(); b(); b();
                    break;
                }

                c(); c(); c(); c(); c();
            }
        """
        self.assertEqual(lint(text), [])

    def test_a_blank_line_inside_parentheses_ends_no_stanza(self):
        text = "void f() {\n    a(1,\n\n      2);\n" + "    b();\n" * 6 + "}\n"
        self.assertEqual(lint(text), [(False, "the stanza from line 2 holds 7 statements")])

    def test_function_lambda_and_inner_blocks_hold_statements(self):
        statements = "    a();\n" * 7
        cases = [
            f"class c {{\n    void f() {qualifier} {{\n{statements}    }}\n}};\n"
            for qualifier in ("const", "volatile", "noexcept", "override", "final", "&", "&&")
        ]
        cases += (
            f"c::c(int n)\n    : m_n(n),\n      m_m(n) {{\n{statements}}}\n",
            f"TEST(suite, name) {{\n{statements}}}\n",
            f"auto f() -> int {{\n{statements}}}\n",
            f"struct s* f(void) {{\n{statements}}}\n",
            f"void f() {{\n    run([&] {{\n{statements}    }});\n}}\n",
            f"void f() {{\n    auto g = [n]() mutable {{\n{statements}    }};\n}}\n",
            f"void f() {{\n    RUN_ELEVATED({{\n{statements}    }});\n}}\n",
            f"void f() {{\n    do {{\n{statements}    }} while (n);\n}}\n",
            f"void f() {{\n    {{\n{statements}    }}\n}}\n",
            f"void f() {{\n    switch (n) {{\n    case 1: {{\n{statements}    }}\n    }}\n}}\n",
            f"void f() {{\n    switch (n) {{\n    case (A ? 1 : 2): {{\n{statements}    }}\n    }}\n}}\n",
        )

        for text in cases:
            with self.subTest(text):
                self.assertEqual([is_error for is_error, _ in lint(text)], [False])

    def test_namespace_type_and_initializer_blocks_hold_no_statements(self):
        declarations = "    int a;\n" * 7
        cases = (
            f"namespace n {{\n{declarations}}}\n",
            f"struct s {{\n{declarations}}};\n",
            f"struct __attribute__((packed)) {{\n{declarations}}} s;\n",
            f"struct alignas(16) {{\n{declarations}}} s;\n",
            "void f() {\n    struct __attribute__((aligned(8))) {\n" + "        int a;\n" * 7 + "    } s;\n}\n",
            "enum e {\n" + "    A,\n" * 7 + "};\n",
            "static const int TABLE[] = {\n" + "    1,\n" * 7 + "};\n",
            f"class c {{\n{declarations}    void f() const {{\n        a();\n    }}\n}};\n",
        )

        for text in cases:
            with self.subTest(text):
                self.assertEqual(lint(text), [])

    def test_a_guard_block_needs_a_blank_line_after_it(self):
        guard = "void f(int n) {{\n    if (!n) {{\n        {}\n    }}\n    a();\n}}\n"
        for exit_statement in ("return;", "continue;", "break;", "goto out;"):
            with self.subTest(exit_statement):
                expected = [(True, "no blank line after the early-exit guard block ending on line 4")]
                self.assertEqual(lint(guard.format(exit_statement)), expected)

        lambda_guard = "void f() {\n    if (any([&] { return a(); })) {\n        return;\n    }\n    b();\n}\n"
        expected = [(True, "no blank line after the early-exit guard block ending on line 4")]
        self.assertEqual(lint(lambda_guard), expected)

        labeled_guard = "void f() {\n    switch (k) {\n    case kind::a:\n        if (!n) {\n            return;\n"
        labeled_guard += "        }\n        b();\n    }\n}\n"
        expected = [(True, "no blank line after the early-exit guard block ending on line 6")]
        self.assertEqual(lint(labeled_guard), expected)

        unclosed_body = "void f(int n) {\n    if (!n) {\n        return;\n    }\n    a();\n"
        expected = [(True, "no blank line after the early-exit guard block ending on line 4")]
        self.assertEqual(lint(unclosed_body), expected)

        separated = (
            "void f(int n) {\n    if (!n) {\n        return;\n    }\n\n    a();\n}\n",
            "void f(int n) {\n    while (n) {\n        if (!n) {\n            break;\n        }\n    }\n}\n",
            "void f(int n) {\n    if (!n) {\n        return;\n    } else {\n        a();\n    }\n}\n",
            "void f(int n) {\n    if (!n) {\n        return;\n    }\n    else {\n        a();\n    }\n}\n",
            "void f(int n) {\n    if (n) {\n        a();\n    }\n    b();\n}\n",
            "void f(int n) {\n    if (n) {\n        a();\n    } else if (!n) {\n        return;\n    }\n    b();\n}\n",
            "void f(int n) {\n#if X\n    if (!n) {\n        return;\n    }\n#endif\n    a();\n}\n",
            "void f(int n) {\n    switch (n) {\n    case 1:\n        if (!n) {\n            break;\n        }\n"
            "    case 2:\n        break;\n    }\n}\n",
        )

        for text in separated:
            with self.subTest(text):
                self.assertEqual([problem for is_error, problem in lint(text) if is_error], [])

    def test_a_guard_in_a_body_of_three_lines_needs_no_blank_line(self):
        short_body = "int f(int x) {\n    if (!x) { return 0; }\n    a();\n    return g(x);\n}\n"
        long_body = "int f(int x) {\n    if (!x) { return 0; }\n    a();\n    a();\n    return g(x);\n}\n"

        self.assertEqual(lint(short_body), [])
        self.assertEqual(lint(long_body), [(True, "no blank line after the early-exit guard block ending on line 2")])

    def test_a_closed_block_of_several_lines_needs_a_blank_line_after_it(self):
        joined = "void f(int n) {\n    while (n) {\n        n--;\n    }\n    a();\n}\n"
        one_line = "void f(int n) {\n    while (n) { n--; }\n    a();\n}\n"
        lambda_statement = "void f(int n) {\n    auto g = [&] {\n        a();\n    };\n    g();\n}\n"
        functions = "void f() {\n    a();\n}\nvoid g() {\n    b();\n}\n"
        after_guard = "void f(int n) {\n    if (!n) {\n        return;\n    }\n    a();\n    while (n) {\n"
        after_guard += "        n--;\n    }\n    b();\n}\n"
        after_guard_findings = [
            (True, "no blank line after the early-exit guard block ending on line 4"),
            (False, "no blank line after the block ending on line 8"),
        ]

        self.assertEqual(lint(joined), [(False, "no blank line after the block ending on line 4")])
        self.assertEqual(lint(one_line), [])
        self.assertEqual(lint(lambda_statement), [])
        self.assertEqual(lint(functions), [])
        self.assertEqual(lint(after_guard), after_guard_findings)

    def test_a_block_of_statements_neither_starts_nor_ends_with_a_blank_line(self):
        starting = "void f() {\n\n    a();\n    b();\n}\n"
        ending = "void f() {\n    a();\n    b();\n\n}\n"

        self.assertEqual(lint(starting), [(False, "a blank line at the start of a block of statements")])
        self.assertEqual(lint(ending), [(False, "a blank line at the end of a block of statements")])
        self.assertEqual(lint("void f() {\n\n}\n"), [(False, "a blank line at the start of a block of statements")])
        self.assertEqual(lint("void f() { a();\n\n    b();\n    c();\n}\n"), [])
        self.assertEqual(lint("void f() {\n    a();\n    b();\n\n    c(); }\n"), [])
        self.assertEqual(lint("namespace n {\n\nint a;\n\n}\n"), [])

    def test_a_body_of_three_lines_holds_no_blank_line(self):
        split = "int f() {\n    int x = g();\n\n    return x;\n}\n"
        joined = "int f() {\n    int x = g();\n    return x;\n}\n"
        labels = "int f(int n) {\n    switch (n) {\n    case 1: return 1;\n    default: return 0;\n    }\n}\n"

        self.assertEqual(lint(split), [(True, "a blank line in a body of at most 3 lines")])
        self.assertEqual(lint(joined), [])
        self.assertEqual(lint(labels), [])

    def test_exits_and_labels_are_read_as_whole_words(self):
        assignment = "void f(int x) {{\n    a();\n    if (x) {{\n        {}\n    }}\n    b();\n}}\n"
        guard = "void f(int x) {{\n    a();\n    if (!x) {{\n        return;\n    }}\n    {}\n}}\n"
        block_finding = [(False, "no blank line after the block ending on line 5")]
        guard_finding = [(True, "no blank line after the early-exit guard block ending on line 5")]

        self.assertEqual(lint(assignment.format("return_code = 1;")), block_finding)
        self.assertEqual(lint(guard.format("case_count = 0;")), guard_finding)
        self.assertEqual(lint(guard.format("default_timeout = 0;")), guard_finding)

    def test_two_blank_lines_in_a_function_body_are_an_error(self):
        expected = [(True, "two blank lines in a row in a function body")]
        self.assertEqual(lint("void f() {\n    a();\n\n\n    b();\n}\n"), expected)
        self.assertEqual(lint("void f() {\n    int t[] = {\n        1,\n\n\n        2,\n    };\n}\n"), expected)
        self.assertEqual(lint("void f() {\n    a();\n    a();\n\n    b();\n}\n"), [])

    def test_two_blank_lines_outside_function_bodies_warn(self):
        expected = [(False, "two blank lines in a row outside a function body")]
        self.assertEqual(lint("int a;\n\n\nint b;\n"), expected)
        self.assertEqual(lint("struct s {\n    int a;\n\n\n    int b;\n};\n"), expected)
        self.assertEqual(lint("int a;\n\n"), [])

    def test_macro_bodies_count_no_statements(self):
        text = "#define MANY() \\\n    void f() { \\\n" + "        a(); \\\n" * 7 + "    }\nint b;\n"
        self.assertEqual(lint(text), [])

    def test_findings_are_reported_only_on_added_lines(self):
        long_stanza = "void f() {\n" + "    a();\n" * 7 + "}\n"
        nested = "void f() {\n" + "    a();\n" * 6 + "    // c\n    a();\n    if (n) {\n        b();\n    }\n}\n"
        guard = "void f(int n) {\n    if (!n) {\n        return;\n    }\n    b();\n}\n"
        double_blank = "void f() {\n    a();\n\n\n    b();\n}\n"
        blank_at_start = "void f() {\n\n    a();\n    b();\n    c();\n}\n"
        blank_at_end = "void f() {\n    a();\n    b();\n    c();\n\n}\n"
        short_body = "void f() {\n    a();\n\n    b();\n}\n"
        cases = (
            (long_stanza, {5, 6}, [5]),
            (long_stanza, {2}, [2]),
            (long_stanza, {8}, [8]),
            (long_stanza, {1, 9}, []),
            (nested, {12}, [12]),
            (nested, {8, 11}, []),
            (guard, {4, 5}, [5]),
            (guard, {4}, [4]),
            (guard, {2, 3}, []),
            (double_blank, {4}, [4]),
            (double_blank, {3, 5}, []),
            (blank_at_start, {2}, [2]),
            (blank_at_start, {1, 3}, []),
            (blank_at_end, {5}, [5]),
            (blank_at_end, {4, 6}, []),
            (short_body, {3}, [3]),
            (short_body, {1, 5}, []),
        )

        for text, added, numbers in cases:
            with self.subTest(text, added=added):
                findings = style_lint.check_stanzas("example.cpp", style_lint.scan_file("example.cpp", text), added)
                self.assertEqual([finding.number for finding in findings], numbers)


class BoundaryTest(unittest.TestCase):
    # Forward declarations never count and `tests/` counts only where it alone defines a namespace, so `heap` and `pmm`
    # belong to `mm/` and `sched` to `sched/`, by its name, though `fs/`, `tests/` and `syscall/` open them more often
    KERNEL_FILES = {
        "kernel/fs/fs.h": "namespace heap { struct block; }\nnamespace mm { struct mm_context; }\nnamespace fs {\n}\n",
        "kernel/fs/file.h": "namespace heap {\nstruct page;\nenum class kind : uint8_t;\n}\n",
        "kernel/mm/heap.h": "namespace heap {\nvoid* kalloc(size_t bytes);\n}\n",
        "kernel/mm/pmm_types.h": "#define NS namespace pmm_macro {\nnamespace pmm {\nusing zone_mask_t = int;\n}\n",
        "kernel/syscall/handlers.h": "namespace sched {\nvoid on_syscall();\n}\n",
        "kernel/syscall/table.h": "namespace sched {\nvoid on_table();\n}\n",
        "kernel/rc/strong_ref.h": "namespace rc {\ntemplate <typename T> class strong_ref {};\n}\n",
        "kernel/tests/mm/pmm.test.cpp": "namespace pmm {\nint check() { return 0; }\n}\n",
        "kernel/tests/mm/zones.test.cpp": "namespace pmm {\nint check_zones() { return 0; }\n}\n",
        "kernel/tests/framework/test_framework.h": "namespace stlx_test {\nvoid run_all();\n}\n",
        "kernel/sched/task.h": "namespace sched {\nstruct task {};\n}\nclass task_namespace final { int id; };\n",
        "kernel/sync/spinlock.h": "namespace sync {\nstruct spinlock {};\n}\n",
        "kernel/common/list.h": "namespace list {\ntemplate <typename T> struct head {};\n}\n",
        "kernel/drivers/usb/xhci/xhci.h": "namespace drivers {\nclass xhci {};\n}\n",
        "kernel/arch/x86_64/defs/segments.h": "namespace x86 {\nconstexpr int KERNEL_CS = 8;\n}\n",
        "kernel/arch/x86_64/trap/idt.h": "namespace x86 {\nvoid load_idt();\n}\n",
        "kernel/arch/x86_64/mm/paging_arch.h": "namespace paging {\nvoid flush();\n}\n",
        "kernel/arch/aarch64/mm/paging_arch.h": "namespace paging {\nvoid flush();\n}\n",
        "kernel/arch/x86_64/sched/fpu.h": "namespace fpu {\nvoid save();\n}\n",
        "kernel/arch/aarch64/trap/fpu_trap.h": "namespace fpu {\nvoid on_trap();\n}\n",
        "kernel/arch/arch_init.h": "namespace arch {\nvoid init();\n}\n",
        "kernel/trap/trap.h": (
            "namespace trap {\n}\nnamespace outer {\nstruct s {};\nnamespace inner {\nvoid f();\n}\n}\n"
        ),
    }

    def flag(self, text: str, path: str) -> list[tuple[int, str]]:
        """Returns each foreign namespace finding in `text`, every line counted as added, as its line and problem."""
        layout = style_lint.build_kernel_layout(self.KERNEL_FILES)
        lines = style_lint.scan_file(path, text)
        findings = style_lint.find_foreign_namespaces(path, lines, {line.number for line in lines}, layout)
        return [(finding.number, finding.message.split(",")[0]) for finding in findings]

    def test_a_namespace_belongs_to_the_subsystem_named_after_it_else_to_one_defining_it(self):
        homes = style_lint.build_kernel_layout(self.KERNEL_FILES).namespace_homes
        subsystems = ("fs", "mm", "syscall", "rc", "tests", "sched", "sync", "common", "drivers", "arch", "trap")
        expected = {name: name for name in subsystems}
        expected |= {
            "heap": "mm",
            "pmm": "mm",
            "stlx_test": "tests",
            "sched": "sched",
            "sync": "sync",
            "list": "common",
            "drivers": "drivers",
            "x86": "arch",
            "paging": "mm",
            "fpu": "arch",
            "arch": "arch",
            "outer": "trap",
        }
        self.assertEqual(homes, expected)

    def test_a_kernel_header_naming_another_subsystems_namespace_is_flagged(self):
        xhci = "kernel/drivers/usb/xhci/xhci_mem.h"
        uses = "int a(pmm::zone_mask_t);\nvoid* b(size_t n) { return ::heap::kalloc(n); }\nint c(pmm::zone_mask_t);\n"
        xhci_findings = [
            (1, "a header of `drivers/` uses `pmm::` from `mm/`"),
            (2, "a header of `drivers/` uses `heap::` from `mm/`"),
        ]
        cases = (
            (xhci, uses, xhci_findings),
            ("kernel/fs/fs.h", "namespace mm { struct context; }\n", [(1, "a header of `fs/` uses `mm::` from `mm/`")]),
            ("kernel/fs/fs.h", "using namespace sched;\n", [(1, "a header of `fs/` uses `sched::` from `sched/`")]),
        )

        for path, text, expected in cases:
            with self.subTest(text):
                self.assertEqual(self.flag(text, path), expected)

    def test_a_namespace_is_reported_on_the_first_added_line_naming_it(self):
        path = "kernel/drivers/usb/xhci/xhci_mem.h"
        lines = style_lint.scan_file(path, "int a;\nint b(pmm::zone_mask_t);\nint c(pmm::zone_mask_t);\n")
        layout = style_lint.build_kernel_layout(self.KERNEL_FILES)

        findings = style_lint.find_foreign_namespaces(path, lines, {2, 3}, layout)
        self.assertEqual([finding.number for finding in findings], [2])
        self.assertEqual(style_lint.find_foreign_namespaces(path, lines, {3}, layout), [])
        self.assertEqual(style_lint.find_foreign_namespaces(path, lines, set(), layout), [])

    def test_a_namespace_the_header_already_names_is_not_flagged(self):
        path = "kernel/drivers/usb/xhci/xhci_mem.h"
        lines = style_lint.scan_file(path, "int a(pmm::zone_mask_t);\nint b(pmm::zone_mask_t);\nheap::block* c;\n")
        layout = style_lint.build_kernel_layout(self.KERNEL_FILES)

        findings = style_lint.find_foreign_namespaces(path, lines, {2, 3}, layout)
        expected = [(3, "a header of `drivers/` uses `heap::` from `mm/`")]
        self.assertEqual([(finding.number, finding.message.split(",")[0]) for finding in findings], expected)

    def test_own_shared_and_unknown_namespaces_are_not_flagged(self):
        text = (
            "drivers::xhci* a;\nsync::spinlock b;\nlist::head<int> c;\nstd::size_t d;\nusb::pmm::port e;\n"
            "// pmm::zone_mask_t\nconst char* f = \"pmm::\";\nauto g = h.pmm::x;\nauto i = j->pmm::x;\n"
            "rc::strong_ref<int> k;\n"
        )
        self.assertEqual(self.flag(text, "kernel/drivers/usb/xhci/xhci_mem.h"), [])

    def test_headers_of_the_unchecked_subsystems_are_not_flagged(self):
        cases = (
            ("kernel/sync/mutex.h", "heap::block* a;\n", []),
            ("kernel/rc/strong_ref.h", "heap::block* a;\n", []),
            ("kernel/tests/framework/helpers.h", "heap::block* a;\n", []),
            ("kernel/common/x.h", "heap::block* a;\n", [(1, "a header of `common/` uses `heap::` from `mm/`")]),
            ("kernel/fs/file.h", "stlx_test::case* a;\n", [(1, "a header of `fs/` uses `stlx_test::` from `tests/`")]),
        )

        for path, text, expected in cases:
            with self.subTest(path):
                self.assertEqual(self.flag(text, path), expected)

    def test_an_arch_tree_directory_belongs_to_the_subsystem_it_mirrors_or_to_arch(self):
        subsystems = style_lint.build_kernel_layout(self.KERNEL_FILES).subsystems
        cases = (
            ("kernel/arch/x86_64/mm/paging_arch.h", "mm"),
            ("kernel/arch/x86_64/defs/segments.h", "arch"),
            ("kernel/arch/x86_64/trap/idt.h", "trap"),
            ("kernel/arch/x86_64/cpu.h", "arch"),
            ("kernel/arch/arch_init.h", "arch"),
            ("kernel/fs/fs.h", "fs"),
            ("kernel/README", None),
            ("userland/lib/libx/x.h", None),
        )

        for path, subsystem in cases:
            with self.subTest(path):
                self.assertEqual(style_lint.find_subsystem(path, subsystems), subsystem)

        idt = "kernel/arch/x86_64/trap/idt.h"
        paging = "kernel/arch/x86_64/mm/paging_arch.h"
        uses = (
            (idt, "x86::segment a;\narch::init();\n", []),
            (idt, "paging::flush();\n", [(1, "a header of `trap/` uses `paging::` from `mm/`")]),
            (paging, "sched::task* a;\n", [(1, "a header of `mm/` uses `sched::` from `sched/`")]),
            ("kernel/drivers/x.h", "x86::segment a;\n", [(1, "a header of `drivers/` uses `x86::` from `arch/`")]),
        )

        for path, text, expected in uses:
            with self.subTest(path, text=text):
                self.assertEqual(self.flag(text, path), expected)

    def test_only_kernel_headers_are_checked(self):
        with unittest.mock.patch.object(style_lint, "read_kernel_layout", side_effect=AssertionError("layout read")):
            for path in ("kernel/drivers/usb/xhci/xhci.cpp", "userland/lib/libx/x.h", "a.h"):
                with self.subTest(path):
                    self.assertEqual(lint("pmm::zone_mask_t a;\n", path), [])


class DiffTest(unittest.TestCase):
    def test_hunks_count_context_lines_and_must_match_their_header(self):
        diff = "diff --git a/x.c b/x.c\n--- a/x.c\n+++ b/x.c\n@@ -1,3 +1,4 @@\n one\n-two\n+TWO\n three\n+four\n"
        self.assertEqual(style_lint.parse_added_lines(diff), {"x.c": {2, 4}})

        for broken in (diff.replace("+1,4", "+1,5"), diff.replace("-1,3", "-1,2"), diff + "+five\n"):
            with self.subTest(broken), self.assertRaises(style_lint.LintError):
                style_lint.parse_added_lines(broken)

    def test_a_missing_final_newline_inside_a_hunk_is_skipped(self):
        diff = "+++ b/x.c\n@@ -1 +1,2 @@\n-one\n\\ No newline at end of file\n+one\n+two\n"
        self.assertEqual(style_lint.parse_added_lines(diff), {"x.c": {1, 2}})

    def test_deleted_files_add_nothing_and_quoted_paths_stop_the_run(self):
        self.assertEqual(style_lint.parse_added_lines("+++ /dev/null\n@@ -1 +0,0 @@\n-one\n"), {})

        with self.assertRaises(style_lint.LintError):
            style_lint.parse_added_lines("+++ \"b/a\\\"b.c\"\n@@ -0,0 +1 @@\n+one\n")


class RepositoryTest(unittest.TestCase):
    """Runs the tool in a scratch repository whose style guide covers `kernel/`, with `kernel/vendor/` vendored."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)

        self.addCleanup(os.chdir, os.getcwd())
        os.chdir(directory.name)

        git("init", "--quiet")
        write(".cursor/rules/style.mdc", "---\nglobs: kernel/**\n---\n")
        write(".gitattributes", "kernel/vendor/** linguist-vendored\nkernel/vendor/own.c -linguist-vendored\n")
        write("kernel/my file.c", "one\ntwo\nthree\n")
        write("kernel/old.c", "// caf\u00e9\n")

        git("add", ".")
        git("commit", "--quiet", "--message", "base")
        self.base = git("rev-parse", "HEAD").strip()

    def run_style_lint(self, directory: str = ".", **environment: str) -> subprocess.CompletedProcess:
        command = [sys.executable, str(SCRIPT), "--base", self.base]
        # Annotations stay off unless a test asks, so scratch findings never reach the pull request running the tests
        variables = {**os.environ, "GITHUB_ACTIONS": "false", **environment}
        return subprocess.run(command, cwd=directory, env=variables, capture_output=True, text=True)

    def test_diff_settings_do_not_change_the_lines_found(self):
        """Each setting changes what `git diff` prints, so the tool must override it."""
        write("kernel/my file.c", "one\nTWO\nthree\nfour\n")
        git("commit", "--quiet", "--all", "--message", "edit")
        write("kernel/my file.c", "one\nTWO\nthree\nfour\nfive\n")
        write("kernel/new.c", "six\n")

        settings = (
            ("diff.noprefix", "true"),
            ("diff.mnemonicPrefix", "true"),
            ("diff.interHunkContext", "9"),
            ("color.diff", "always"),
            ("diff.external", "true"),
        )

        for setting, value in settings:
            with self.subTest(setting):
                git("config", setting, value)
                added = style_lint.collect_added_lines(self.base, ["kernel/**"])
                git("config", "--unset", setting)

                self.assertEqual(added, {"kernel/my file.c": {2, 4, 5}, "kernel/new.c": None})

    def test_checks_globbed_first_party_files_of_the_types_it_reads(self):
        for path in ("kernel/a.c", "kernel/vendor/b.c", "kernel/vendor/own.c", "kernel/notes.md", "tools/c.c"):
            write(path, "// caf\u00e9\n")

        result = self.run_style_lint()
        output = [
            f"  error kernel/a.c:1: {E_ACUTE} in a comment, comments must be ASCII",
            f"  error kernel/vendor/own.c:1: {E_ACUTE} in a comment, comments must be ASCII",
            "style_lint: 2 file(s) checked, 2 error(s), 0 warning(s)",
        ]
        self.assertEqual((result.returncode, result.stdout.splitlines()), (1, output))

    def test_checks_only_the_lines_the_change_adds(self):
        write("kernel/old.c", "// caf\u00e9\n// na\u00efve\n")

        result = self.run_style_lint()
        output = [
            f"  error kernel/old.c:2: {I_DIAERESIS} in a comment, comments must be ASCII",
            "style_lint: 1 file(s) checked, 1 error(s), 0 warning(s)",
        ]
        self.assertEqual((result.returncode, result.stdout.splitlines()), (1, output))

    def test_a_renamed_file_adds_no_lines(self):
        git("mv", "kernel/old.c", "kernel/renamed.c")
        git("commit", "--quiet", "--message", "rename")

        result = self.run_style_lint()
        summary = "style_lint: 0 file(s) checked, 0 error(s), 0 warning(s)"
        self.assertEqual((result.returncode, result.stdout.splitlines()), (0, [summary]))

    def test_runs_from_a_subdirectory(self):
        write("kernel/a.c", "// caf\u00e9\n")

        result = self.run_style_lint("kernel")
        summary = "style_lint: 1 file(s) checked, 1 error(s), 0 warning(s)"
        self.assertEqual((result.returncode, result.stdout.splitlines()[-1]), (1, summary))

    def test_flags_a_kernel_header_naming_another_subsystems_namespace(self):
        write("kernel/mm/pmm.h", "namespace pmm {\nusing zone_mask_t = unsigned;\n}\n")
        write("kernel/drivers/x.h", "pmm::zone_mask_t zones();\n")

        result = self.run_style_lint()
        output = [
            "  warning kernel/drivers/x.h:1: a header of `drivers/` uses `pmm::` from `mm/`, keep it if the header's "
            "role needs `mm/`'s types, else use `drivers/`'s own types and convert them to `mm/`'s in its source file",
            "style_lint: 2 file(s) checked, 0 error(s), 1 warning(s)",
        ]
        self.assertEqual((result.returncode, result.stdout.splitlines()), (0, output))

    def test_warnings_alone_exit_with_0(self):
        write("kernel/a.c", "// TODO drop the retry\n")
        self.assertEqual(self.run_style_lint().returncode, 0)

    def test_annotates_errors_and_warnings_in_github_actions(self):
        write("kernel/a.c", "// caf\u00e9\n// TODO drop the retry\n")

        result = self.run_style_lint(GITHUB_ACTIONS="true")
        annotations = [line for line in result.stdout.splitlines() if line.startswith("::")]
        expected = [
            f"::error file=kernel/a.c,line=1::{E_ACUTE} in a comment, comments must be ASCII",
            "::warning file=kernel/a.c,line=2::'TODO' marks unfinished work, keep comments to what is true",
        ]
        self.assertEqual(annotations, expected)

    def test_exits_with_2_when_the_style_guide_names_no_files(self):
        for globs_line in ("description: no globs", "globs: \"kernel/**\"", "globs: [kernel/**]"):
            with self.subTest(globs_line):
                write(".cursor/rules/style.mdc", f"---\n{globs_line}\n---\n")
                self.assertEqual(self.run_style_lint().returncode, 2)

        os.remove(".cursor/rules/style.mdc")
        self.assertEqual(self.run_style_lint().returncode, 2)

    def test_exits_with_2_when_git_or_a_changed_file_fails(self):
        outside = tempfile.TemporaryDirectory()
        self.addCleanup(outside.cleanup)

        self.assertEqual(self.run_style_lint(outside.name).returncode, 2)
        self.assertEqual(self.run_style_lint(PATH="").returncode, 2)

        os.symlink("missing.c", "kernel/broken.c")
        self.assertEqual(self.run_style_lint().returncode, 2)


if __name__ == "__main__":
    unittest.main()
