#!/usr/bin/env python3
"""Tests for `style_lint.py`, run with `python3 scripts/style_lint_test.py`."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import style_lint

SCRIPT = Path(__file__).resolve().with_name("style_lint.py")
GIT_IDENTITY = ("-c", "user.name=Style Lint", "-c", "user.email=style-lint@example.com", "-c", "commit.gpgsign=false")
E_ACUTE = "U+00E9 (latin small letter e with acute)"
I_DIAERESIS = "U+00EF (latin small letter i with diaeresis)"


def lint(text: str, path: str = "example.cpp") -> list[str]:
    """Returns the problem each finding in `text` names, with every line counted as added."""
    findings = style_lint.check_file(path, text, None)
    return [finding.message.split(",")[0] for finding in findings]


def git(*arguments: str) -> str:
    return subprocess.run(["git", *GIT_IDENTITY, *arguments], check=True, capture_output=True, text=True).stdout


def write(path: str, text: str) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(text)


class ScanTest(unittest.TestCase):
    def test_each_file_type_reads_its_own_comment_syntax(self):
        text = "x /* \u00e9 */ # \u00ef"
        c_comments = [f"{I_DIAERESIS} outside a comment", f"{E_ACUTE} in a comment"]
        hash_comments = [f"{E_ACUTE} outside a comment", f"{I_DIAERESIS} in a comment"]
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
            ("int caf\u00e9 = 0;", f"{E_ACUTE} outside a comment"),
            ("puts(\"caf\u00e9\");", f"{E_ACUTE} outside a comment"),
            ("// caf\u00e9", f"{E_ACUTE} in a comment"),
            ("// one \u2014 two", "U+2014 (em dash) is a typographic dash"),
        )

        for text, problem in cases:
            with self.subTest(text):
                self.assertEqual(lint(text), [problem])

    def test_a_dash_does_not_hide_another_non_ascii_character(self):
        expected = ["U+2192 (rightwards arrow) in a comment", "U+2014 (em dash) is a typographic dash"]
        self.assertEqual(lint("// one \u2014 two \u2192 three"), expected)


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
            "style_lint: 2 file(s) checked, 2 error(s)",
        ]
        self.assertEqual((result.returncode, result.stdout.splitlines()), (1, output))

    def test_checks_only_the_lines_the_change_adds(self):
        write("kernel/old.c", "// caf\u00e9\n// na\u00efve\n")

        result = self.run_style_lint()
        output = [
            f"  error kernel/old.c:2: {I_DIAERESIS} in a comment, comments must be ASCII",
            "style_lint: 1 file(s) checked, 1 error(s)",
        ]
        self.assertEqual((result.returncode, result.stdout.splitlines()), (1, output))

    def test_a_renamed_file_adds_no_lines(self):
        git("mv", "kernel/old.c", "kernel/renamed.c")
        git("commit", "--quiet", "--message", "rename")

        result = self.run_style_lint()
        summary = "style_lint: 0 file(s) checked, 0 error(s)"
        self.assertEqual((result.returncode, result.stdout.splitlines()), (0, [summary]))

    def test_runs_from_a_subdirectory(self):
        write("kernel/a.c", "// caf\u00e9\n")

        result = self.run_style_lint("kernel")
        summary = "style_lint: 1 file(s) checked, 1 error(s)"
        self.assertEqual((result.returncode, result.stdout.splitlines()[-1]), (1, summary))

    def test_annotates_errors_in_github_actions(self):
        write("kernel/a.c", "// caf\u00e9\n")

        result = self.run_style_lint(GITHUB_ACTIONS="true")
        annotations = [line for line in result.stdout.splitlines() if line.startswith("::")]
        expected = [f"::error file=kernel/a.c,line=1::{E_ACUTE} in a comment, comments must be ASCII"]
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
