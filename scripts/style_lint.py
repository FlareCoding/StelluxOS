#!/usr/bin/env python3
"""Checks the lines a change adds against the StelluxOS style guide, `.cursor/rules/style.mdc`.

Comments, code and strings must be ASCII only, as the guide's language rules require. Comments must also follow its
rules on dashes, semicolons, banned words and wording that dates them, and must not draw decorative separators. A few
conventions the guide leaves unwritten, such as keeping `TODO` markers out of comments, are checked as well.

C and C++ code must follow the guide's stanza formatting: a blank line after an early-exit guard block, at most six
statements in a stanza, never two blank lines in a row in a function body, and no blank line in a body of three
lines. Conventions the guide leaves unwritten are checked as well: a closed block of several lines ends its stanza,
no block of statements starts or ends with a blank line, and no two blank lines in a row appear outside function
bodies either.

A kernel header is flagged once for each namespace of another subsystem that its added lines name and its unchanged
lines do not, so that a reviewer can check whether the header's role needs that subsystem's types, or whether the
header should use its own subsystem's types and convert them in its source file. A namespace belongs to the
subsystem of the same name, else to the one whose files define it most often. Any header may use the namespaces of
`common/`, `sync/` and `rc/`, and a header inside an arch tree those of `arch/`. Headers of `sync/`, `rc/` and
`tests/` are not checked, since `sync/` keeps tasks and address spaces in its fields and calls `hw/` inline, `rc/`
calls `mm/` inline, and test headers use what they test.

The files checked are the paths the globs line of the guide's front matter matches, minus the paths `.gitattributes`
marks `linguist-vendored`, which hold third-party code. Files ending in `.c`, `.cpp`, `.h`, `.hpp`, `.S`, `.ld` or
`.inc` are read with C-style comments, and files ending in `.sh` or `.mk` and files named `Makefile` with hash
comments. Other files the globs match are not read. The stanza rules read the `.c`, `.cpp`, `.h` and `.hpp` files,
and the namespace flag the `.h` and `.hpp` files under `kernel/`, after reading every C and C++ file under `kernel/`,
tracked or untracked, to find the subsystem each namespace belongs to. A change is every commit since the merge base
of `HEAD` and the base ref, `origin/master` unless `--base` names another, plus the uncommitted edits. Only the lines
it adds or edits are checked, so untouched code keeps the style it has. Untracked files are checked whole.

Text in backticks names code, and a comment line indented two or more spaces past its marker is a code example
unless it continues a list item or a doxygen tag other than `@code`, so an example under a list item or a tag
needs a lead-in line such as "Usage:". Of the comment checks, only those for non-ASCII characters and typographic
dashes read either.

Breaking a rule the guide writes down is an error and fails the run. A match that may be innocent is a warning, such
as a phrase that may date the comment or a flagged namespace. A stanza of more than the guide's six statements is a
warning too, though the guide writes the rule down, since the stanza reader can misread a block and code the project
accepts often holds longer stanzas. A break of an unwritten convention is a warning as well, such as two blank lines
in a row outside a function body. In GitHub Actions every finding is also printed as an annotation, so errors and
warnings both appear on the pull request.

Usage:
    scripts/style_lint.py                checks the change since the merge base with origin/master
    scripts/style_lint.py --base <ref>   checks the change since the merge base with <ref>

Exits with 0 when no error is found and 1 when one is. Exits with 2 when git fails, the guide cannot be read, its
globs line is missing or holds a glob that matches no tracked file, the diff cannot be parsed, or a changed file
cannot be read.
"""

from __future__ import annotations

import argparse
import functools
import os
import re
import subprocess
import sys
import unicodedata
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from enum import Enum, auto
from pathlib import Path
from typing import Callable, Iterable, Iterator, NamedTuple

STYLE_GUIDE = ".cursor/rules/style.mdc"
GLOBS_LINE = re.compile(r"^globs:[ \t]*(\S.*)$", re.MULTILINE)

C_COMMENT_FILE_TYPES = frozenset({".c", ".cpp", ".h", ".hpp", ".S", ".ld", ".inc"})
HASH_COMMENT_FILE_TYPES = frozenset({".sh", ".mk", "Makefile"})
C_CODE_FILE_TYPES = frozenset({".c", ".cpp", ".h", ".hpp"})

# Keeps the diff in one form whatever the user's settings: no color and no external diff tool, `a/` and `b/` prefixes
# despite `diff.noprefix` or `diff.mnemonicPrefix`, and no context lines joining hunks under `diff.interHunkContext`
DIFF_OPTIONS = (
    "--unified=0",
    "--inter-hunk-context=0",
    "--no-color",
    "--no-ext-diff",
    "--find-renames",
    "--src-prefix=a/",
    "--dst-prefix=b/",
)
HUNK_HEADER = re.compile(r"^@@ -\d+(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")

# A string or character literal up to its closing quote, or to the end of the line when it stays open
STRING_LITERAL = re.compile(r"\"(?:\\.|[^\"\\])*\"?|'(?:\\.|[^'\\])*'?")
BACKTICK_SPAN = re.compile(r"`[^`]*`")
# In prose, each backtick span becomes this one word, so the words around it keep their places
BACKTICK_WORD = "name"
# A list item or a doxygen tag other than `@code`, whose wrapped lines are indented prose rather than code
TAG_OR_LIST_ITEM = re.compile(r"@(?!code\b)\w|- |\d+\. ")

TYPOGRAPHIC_DASHES = "\u2013\u2014"
NON_ASCII = re.compile(r"[^\x00-\x7f]")
# Typographic dashes are reported once, by their own rule
NON_ASCII_EXCEPT_DASHES = re.compile(f"[^\\x00-\\x7f{TYPOGRAPHIC_DASHES}]")
TYPOGRAPHIC_DASH = re.compile(f"[{TYPOGRAPHIC_DASHES}]")
# A spaced hyphen or double hyphen between a word, comma or parenthesis and two letters or a parenthesis, or a lone
# `--`. A digit after it reads as arithmetic, as in `count - 1`, and an unspaced `word--word` goes undetected.
SPACED_DASH = re.compile(r"(?:(?<=[A-Za-z0-9]{2})|(?<=[,)]))\s+--?\s+(?=[A-Za-z]{2}|\()|(?:^|\s)--(?:\s|$)")
SEMICOLON = re.compile(";")
# Stems also catch each banned word's relatives, while "simply" is matched whole so that "simple" passes
BANNED_WORD = re.compile(
    r"\b(?:leverag|robust|seamless|elegan|comprehensive|powerful)\w*|\b(?:simply|cutting-edge)\b",
    re.IGNORECASE,
)

CHANGE_VERB = r"(?:adds|fixes|changes|removes|introduces|makes|moves|updates|replaces)"
# Phrases that only refer to the change or conversation behind a comment. "This commit" with a verb only warns, since
# `stlxwin.h` calls a frame update a commit, and "as discussed above" points within the file.
DATED_PHRASE = re.compile(
    rf"\bthis (?:pr|patch|diff) {CHANGE_VERB}\b|\b(?:in|since|before|after) this (?:pr|patch|diff)\b"
    r"|\bthe refactor(?:ing)?\b|\bfor now\b|\bas discussed\b(?! (?:above|below|earlier|later)\b)",
    re.IGNORECASE,
)
# Phrases that may date a comment but have timeless readings, so they only warn. "In the future" and "we changed"
# count only where a comment line starts, "until ... lands" spans three words at most, and "at the moment of" passes.
MAY_DATE_PHRASE = re.compile(
    r"\bnow (?:uses|returns|handles|supports|calls|takes)\b|\b(?:new|old) (?:code|implementation|approach)\b"
    r"|\bat the moment\b(?! of\b)|^in the future\b"
    r"|^(?:we|i) (?:added|changed|moved|removed|renamed|replaced|updated|fixed|switched|refactored)\b"
    rf"|\bthis commit {CHANGE_VERB}\b|\buntil (?:[\w-]+ ){{1,3}}(?:lands|is (?:implemented|supported|fixed))\b",
    re.IGNORECASE,
)
FILLER_WORD = re.compile(r"\b(?:utili[sz]|delv)(?:e|es|ed|ing)\b", re.IGNORECASE)
WORK_MARKER = re.compile(r"\b(?:TODO|FIXME|XXX|HACK)\b")
# A run of three or more of `-=*#~_/`, alone or around a title. A line holding a `|` or `+` is a table or a box and
# does not match.
SEPARATOR = re.compile(r"^\s*[-=*#~_/]{3,}(?:[^|+]*[-=*#~_/]{3,})?\s*$")

# The guide's stanzas hold one to six statements. A longer one only warns, since `StanzaReader` can misread a block,
# as it does both branches of an `#if`, and code the project accepts often holds longer stanzas.
MAX_STANZA_STATEMENTS = 6
# The guide's short bodies span one to three lines and form a single stanza
MAX_SHORT_BODY_LINES = 3
# The end of a function, lambda or control statement header before its `{`: a closing parenthesis or a lambda
# capture's bracket, then any qualifiers or trailing return type
FUNCTION_HEADER = r"[)\]](?:\s*(?:const|volatile|noexcept|override|final|mutable|&&?|->\s*[\w:<>*&, ]+))*\s*$"
FUNCTION_BODY_HEADER = re.compile(FUNCTION_HEADER)
# Inside a function, a block whose header is empty or ends in `else`, `do` or `(` also holds statements, such as a
# plain scope, a `case` block or a `RUN_ELEVATED({ ... })` body, but not an initializer or a local `struct`.
STATEMENT_BLOCK_HEADER = re.compile(rf"{FUNCTION_HEADER}|(?:^|\b(?:else|do)|\()\s*$")
# Attributes end in a parenthesis, which makes a header such as `struct __attribute__((packed))` read as a function's
ATTRIBUTE = re.compile(r"__attribute__\s*\(\(.*\)\)|alignas\s*\([^)]*\)")
# An `else` after a closing brace starts the header of the statement's next branch
ELSE_BRANCH = re.compile(r"else\b")
CASE_LABEL = re.compile(r"(?:case\b|default\s*:)")
GUARD_HEADER = re.compile(r"if\b")
EARLY_EXIT = re.compile(r"(?:return|continue|break|goto)\b")

HEADER_FILE_TYPES = frozenset({".h", ".hpp"})
# A namespace that code names, as in `pmm::zone_mask_t` or `::mm::`, but not a later part of a qualified name
NAMESPACE_USE = re.compile(r"(?<![\w:.>])(?:::)?([A-Za-z_]\w*)::")
# A namespace that code opens, forward-declares into or brings in with `using namespace`
NAMESPACE_NAMED = re.compile(r"\bnamespace\s+([A-Za-z_]\w*)")
NAMESPACE_BLOCK = re.compile(r"\bnamespace\s+([A-Za-z_]\w*)(?:\s*::\s*\w+)*\s*\{")
# A statement of a namespace block that only forward-declares a type, such as `struct task` or `enum class kind : int`
FORWARD_DECLARATION = re.compile(
    r"\s*(?:(?:struct|class|union|enum(?:\s+class|\s+struct)?)\s+\w+(?:\s*:\s*[\w:]+)?)?\s*"
)
# Subsystems whose namespaces any kernel header may use: `common/` holds what every file uses, and `sync/` and `rc/`
# hold the locks and reference counts any object may embed
SHARED_SUBSYSTEMS = frozenset({"common", "sync", "rc"})
# Subsystems whose headers are not checked: `sync/` keeps tasks and address spaces in its fields and calls `hw/`
# inline, `rc/` calls `mm/` inline, and test headers use what they test
UNCHECKED_SUBSYSTEMS = frozenset({"sync", "rc", "tests"})

# A rule's basis sets its severity: breaking a rule the guide writes down is an error, while a match that may be
# innocent and a break of a convention the guide leaves unwritten are warnings
WRITTEN = "written"
AMBIGUOUS = "ambiguous"
UNWRITTEN = "unwritten"


class Rule(Enum):
    """Every rule a check family reports, with the basis that sets its severity and the message it prints."""

    NON_ASCII_OUTSIDE_COMMENTS = (WRITTEN, "{} outside a comment, code and strings must be ASCII")
    NON_ASCII_IN_COMMENT = (WRITTEN, "{} in a comment, comments must be ASCII")
    TYPOGRAPHIC_DASH = (WRITTEN, "{} is a typographic dash, use a comma, parentheses, or a new sentence")
    SPACED_DASH = (
        WRITTEN,
        "dash as punctuation, use a comma, parentheses, or a new sentence, or backtick it if it is code",
    )
    SEMICOLON = (WRITTEN, "semicolon in a comment, use a comma or split the sentence")
    BANNED_WORD = (WRITTEN, "{} is a banned word, use a plain and specific one")
    DATED_PHRASE = (WRITTEN, "{} dates the comment, say what is always true")
    SEPARATOR = (WRITTEN, "decorative separator, remove it")
    MAY_DATE_PHRASE = (AMBIGUOUS, "{} may date the comment, say what is always true")
    FILLER_WORD = (UNWRITTEN, "{} is filler, use a plain word")
    WORK_MARKER = (UNWRITTEN, "{} marks unfinished work, keep comments to what is true")
    GUARD_WITHOUT_BLANK = (
        WRITTEN,
        "no blank line after the early-exit guard block ending on line {}, add one after its closing brace",
    )
    DOUBLE_BLANK = (WRITTEN, "two blank lines in a row in a function body, separate stanzas with exactly one")
    DOUBLE_BLANK_OUTSIDE_BODIES = (UNWRITTEN, "two blank lines in a row outside a function body, use one")
    LONG_STANZA = (
        AMBIGUOUS,
        f"the stanza from line {{}} holds {{}} statements, split it into stanzas of at most {MAX_STANZA_STATEMENTS}",
    )
    SHORT_BODY_BLANK = (
        WRITTEN,
        f"a blank line in a body of at most {MAX_SHORT_BODY_LINES} lines, remove it to make the body one stanza",
    )
    BLOCK_WITHOUT_BLANK = (
        UNWRITTEN,
        "no blank line after the block ending on line {}, add one after its closing brace",
    )
    BLANK_AT_BLOCK_EDGE = (UNWRITTEN, "a blank line at the {} of a block of statements, remove it")
    FOREIGN_NAMESPACE = (
        AMBIGUOUS,
        "a header of {0} uses `{1}::` from {2}, keep it if the header's role needs {2}'s types, else use {0}'s own "
        "types and convert them to {2}'s in its source file",
    )

    def __init__(self, basis: str, message: str):
        self.message = message
        self.is_error = basis == WRITTEN


class LinePart(Enum):
    """The part of a scanned line that a line check searches.

    Prose is the comment with each backtick span read as one word, and a code example has none.
    """

    OUTSIDE_COMMENTS = auto()
    COMMENT = auto()
    PROSE = auto()


class ScannedLine(NamedTuple):
    """One line of a file as written, split into its comment, its string literals, and its code."""

    number: int
    raw: str
    comment: str | None  # comment text, with the spaces after a `//`, `*` or `#` kept, or `None` without a comment
    strings: str         # the string and character literals, quotes included, separated by spaces
    code: str            # text outside comments, with each literal emptied to its quotes


class Finding(NamedTuple):
    """A rule that one line of a file breaks."""

    path: str
    number: int
    rule: Rule
    message: str


class LineCheck(NamedTuple):
    """A pattern that breaks a rule wherever it appears in one part of a line."""

    rule: Rule
    part: LinePart
    pattern: re.Pattern


class CheckFamily(NamedTuple):
    """Checks that run together over one file's scanned lines, for the file types they name."""

    file_types: frozenset[str]
    check: Callable[[str, list[ScannedLine], set[int]], list[Finding]]


class Stanza(NamedTuple):
    """A run of lines in one block of statements and the lines its statements end on.

    A blank line between statements, a `case` label, a `default` label or the block's closing brace ends it.
    """

    first: int
    statement_lines: list[int]  # the line each statement ends on, once per statement


@dataclass
class OpenBlock:
    """A brace block while its lines are read, with what the stanza rules judge once it closes.

    A block holds statements when it is the body of a function or lambda. So does the body of a control statement, a
    plain scope, a block after a `case` label or a block opened right after a `(` inside one, such as a
    `RUN_ELEVATED({ ... })` body or the braced argument of `f({a, b})`. Other blocks, such as a namespace, a type or
    an initializer after `=`, hold none.
    """

    header: str                 # the code before the block's `{`, such as `if (n)` or `else`
    holds_statements: bool
    paren_depth: int            # how many parentheses are open at the `{`, the depth at which a `;` ends a statement
    opened_on: int              # the line of the block's `{`
    statement: str = ""         # the code read so far of the statement in progress at the block's own level
    last_statement: str = ""    # the last statement's code, or a control statement's header
    stanza_first: int | None = None
    stanza_statement_lines: list[int] = field(default_factory=list)
    stanzas: list[Stanza] = field(default_factory=list)
    blank_lines: list[int] = field(default_factory=list)         # blank lines between the block's statements
    guard_findings: list[Finding] = field(default_factory=list)  # its guards' findings, held until its length is known

    def touch(self, number: int) -> None:
        """Starts a stanza at line `number` unless one is in progress."""
        if self.stanza_first is None:
            self.stanza_first = number

    def end_statement(self, text: str, number: int) -> None:
        """Counts a statement ending on line `number` in the stanza in progress."""
        self.last_statement = text.strip()
        self.statement = ""
        self.stanza_statement_lines.append(number)

    def end_stanza(self) -> None:
        """Closes the stanza in progress, if one is."""
        if self.stanza_first is not None:
            self.stanzas.append(Stanza(self.stanza_first, self.stanza_statement_lines))

        self.stanza_first = None
        self.stanza_statement_lines = []


class KernelLayout(NamedTuple):
    """The kernel's subsystems, which are the directories under `kernel/`, and the subsystem of each namespace."""

    subsystems: frozenset[str]
    namespace_homes: dict[str, str]


class LintError(Exception):
    """A failure that stops the run, such as git failing."""


class StanzaReader:
    """Reads a C or C++ file into its brace blocks and judges the stanzas of each block of statements.

    A statement ends at a `;` outside the parentheses opened in its block or at the closing brace of a block of
    statements, so a control statement counts once whatever its body holds. Comments and preprocessor lines join a
    stanza without counting, and a `case` or `default` label starts a new one.
    """

    def __init__(self, path: str, lines: list[ScannedLine], added: set[int]):
        self.path = path
        self.lines = lines
        self.added = added
        self.blocks = [OpenBlock("", False, 0, 0)]
        self.paren_depth = 0
        self.continues_directive = False
        self.follows_blank = False
        self.findings: list[Finding] = []

    def read(self) -> list[Finding]:
        """Reads every line and returns the stanza rules the added lines break, in line order."""
        # `scan_file` gives a file ending in a newline an empty last line, which is not a line of the file
        is_past_end = bool(self.lines) and self.lines[-1].raw == ""
        file_lines = self.lines[:-1] if is_past_end else self.lines

        for line in file_lines:
            self.read_line(line)

        for block in self.blocks:
            self.findings.extend(block.guard_findings)

        return sorted(self.findings, key=lambda finding: finding.number)

    def read_line(self, line: ScannedLine) -> None:
        """Reads a line into the block it is in, character by character when it holds code."""
        code = line.code.strip()
        is_directive = self.continues_directive or code.startswith("#")
        self.continues_directive = is_directive and line.raw.rstrip().endswith("\\")
        block = self.blocks[-1]
        is_own_level = block.holds_statements and self.paren_depth == block.paren_depth

        if not line.raw.strip():
            self.read_blank_line(line, block, is_own_level)
            return

        self.follows_blank = False

        if is_directive or not code:
            block.touch(line.number)
            return

        if is_own_level and CASE_LABEL.match(code):
            block.end_stanza()

        for column, character in enumerate(line.code):
            self.read_character(line, column, character)

        self.blocks[-1].statement += " "

    def read_blank_line(self, line: ScannedLine, block: OpenBlock, is_own_level: bool) -> None:
        """Ends the stanza in progress at a blank line between statements.

        Reports a blank line that starts a block of statements or follows another blank line.
        """
        is_block_start = is_own_level and line.number == block.opened_on + 1 and block.stanza_first is None
        if is_block_start and line.number in self.added:
            self.report(line.number, Rule.BLANK_AT_BLOCK_EDGE, "start")

        if is_own_level:
            block.end_stanza()
            block.blank_lines.append(line.number)

        if self.follows_blank and line.number in self.added:
            is_in_body = any(open_block.holds_statements for open_block in self.blocks)
            self.report(line.number, Rule.DOUBLE_BLANK if is_in_body else Rule.DOUBLE_BLANK_OUTSIDE_BODIES)

        self.follows_blank = True

    def read_character(self, line: ScannedLine, column: int, character: str) -> None:
        """Reads a character of code, opening and closing blocks and ending statements."""
        block = self.blocks[-1]
        if character == "}" and len(self.blocks) > 1:
            self.close_block(line, column)
            return

        if not character.isspace():
            block.touch(line.number)

        block.statement += character

        if character == "(":
            self.paren_depth += 1
        elif character == ")":
            self.paren_depth -= 1
        elif character == "{":
            header = block.statement[:-1].strip()
            header_pattern = STATEMENT_BLOCK_HEADER if block.holds_statements else FUNCTION_BODY_HEADER
            holds_statements = bool(header_pattern.search(ATTRIBUTE.sub("", header)))
            self.blocks.append(OpenBlock(header, holds_statements, self.paren_depth, line.number))
        elif character == ";" and self.paren_depth == block.paren_depth:
            block.end_statement(block.statement, line.number)
        elif character == ":" and self.ends_case_label(line.code, column, block):
            block.statement = ""

    def ends_case_label(self, code: str, column: int, block: OpenBlock) -> bool:
        """Tells whether a `:` ends a `case` or `default` label, so the code after it starts another statement."""
        is_scope = code[column - 1:column] == ":" or code[column + 1:column + 2] == ":"
        is_own_level = block.holds_statements and self.paren_depth == block.paren_depth
        return is_own_level and not is_scope and bool(CASE_LABEL.match(block.statement.lstrip()))

    def close_block(self, line: ScannedLine, column: int) -> None:
        """Judges the innermost block at its closing brace and ends the statement the block belongs to."""
        closed = self.blocks.pop()
        parent = self.blocks[-1]
        # Code after the brace continues the statement, as an `else` does or a `;` after a lambda
        rest = line.code[column + 1:].strip()
        ends_statement = closed.holds_statements and self.paren_depth == parent.paren_depth and not rest

        closed.end_stanza()
        if closed.holds_statements:
            self.judge_body(closed, line, column)
            self.judge_following_line(closed, parent, line.number, ends_statement)

        parent.touch(line.number)
        if ends_statement:
            parent.end_statement(closed.header, line.number)
        elif closed.holds_statements and ELSE_BRANCH.match(rest):
            parent.statement = ""
        else:
            parent.statement += "{}"

    def judge_body(self, block: OpenBlock, line: ScannedLine, column: int) -> None:
        """Reports the stanza rules a block of statements breaks, judged at its closing brace."""
        body_lines = range(block.opened_on + 1, line.number)
        is_short_body = len(body_lines) <= MAX_SHORT_BODY_LINES
        if not is_short_body:
            self.findings.extend(block.guard_findings)

        for stanza in block.stanzas:
            number = find_first_added(stanza.statement_lines, self.added)
            if len(stanza.statement_lines) > MAX_STANZA_STATEMENTS and number:
                self.report(number, Rule.LONG_STANZA, stanza.first, len(stanza.statement_lines))

        last_body_line = line.number - 1
        has_inner_blank = any(block.opened_on + 1 < blank < last_body_line for blank in block.blank_lines)
        number = find_first_added(body_lines, self.added)
        if is_short_body and has_inner_blank and number:
            self.report(number, Rule.SHORT_BODY_BLANK)

        is_brace_first = not line.code[:column].strip()
        is_block_end = last_body_line > block.opened_on + 1 and not self.lines[last_body_line - 1].raw.strip()
        if is_brace_first and is_block_end and last_body_line in self.added:
            self.report(last_body_line, Rule.BLANK_AT_BLOCK_EDGE, "end")

    def judge_following_line(self, block: OpenBlock, parent: OpenBlock, closed_on: int, ends_statement: bool) -> None:
        """Reports a guard block, or a closed block of several lines, with no blank line after it.

        A next line starting with `}`, `#`, `else`, `case` or `default` needs none.
        """
        following = self.lines[closed_on] if closed_on < len(self.lines) else None
        is_multiline = closed_on > block.opened_on
        if not ends_statement or not parent.holds_statements or following is None:
            return

        next_code = following.code.strip()
        is_blank = not following.raw.strip()
        starts_stanza = next_code.startswith(("}", "#")) or bool(CASE_LABEL.match(next_code))
        continues_statement = bool(ELSE_BRANCH.match(next_code))
        is_guard = bool(GUARD_HEADER.match(block.header) and EARLY_EXIT.match(block.last_statement))
        number = find_first_added([closed_on + 1, closed_on], self.added)
        if is_blank or starts_stanza or continues_statement or not number:
            return

        if is_guard:
            message = Rule.GUARD_WITHOUT_BLANK.message.format(closed_on)
            parent.guard_findings.append(Finding(self.path, number, Rule.GUARD_WITHOUT_BLANK, message))
        elif is_multiline:
            self.report(number, Rule.BLOCK_WITHOUT_BLANK, closed_on)

    def report(self, number: int, rule: Rule, *values: object) -> None:
        """Reports a rule broken on line `number`, its message filled with `values`."""
        self.findings.append(Finding(self.path, number, rule, rule.message.format(*values)))


LINE_CHECKS = (
    LineCheck(Rule.NON_ASCII_OUTSIDE_COMMENTS, LinePart.OUTSIDE_COMMENTS, NON_ASCII),
    LineCheck(Rule.NON_ASCII_IN_COMMENT, LinePart.COMMENT, NON_ASCII_EXCEPT_DASHES),
    LineCheck(Rule.TYPOGRAPHIC_DASH, LinePart.COMMENT, TYPOGRAPHIC_DASH),
    LineCheck(Rule.SPACED_DASH, LinePart.PROSE, SPACED_DASH),
    LineCheck(Rule.SEMICOLON, LinePart.PROSE, SEMICOLON),
    LineCheck(Rule.BANNED_WORD, LinePart.PROSE, BANNED_WORD),
    LineCheck(Rule.DATED_PHRASE, LinePart.PROSE, DATED_PHRASE),
    LineCheck(Rule.SEPARATOR, LinePart.PROSE, SEPARATOR),
    LineCheck(Rule.MAY_DATE_PHRASE, LinePart.PROSE, MAY_DATE_PHRASE),
    LineCheck(Rule.FILLER_WORD, LinePart.PROSE, FILLER_WORD),
    LineCheck(Rule.WORK_MARKER, LinePart.PROSE, WORK_MARKER),
)


def find_file_type(path: str) -> str:
    """Returns the suffix that names a file's type, or the whole name of a file without one, such as `Makefile`."""
    name = os.path.basename(path)
    return os.path.splitext(name)[1] or name


def scan_file(path: str, text: str) -> list[ScannedLine]:
    """Splits each line of a file into its comment, its string literals, and its code."""
    uses_c_comments = find_file_type(path) in C_COMMENT_FILE_TYPES
    in_block_comment = False
    lines = []

    for number, raw in enumerate(text.split("\n"), start=1):
        if uses_c_comments:
            comment, outside, in_block_comment = split_c_line(raw, in_block_comment)
        else:
            comment, outside = split_hash_line(raw)

        strings = " ".join(STRING_LITERAL.findall(outside))
        code = STRING_LITERAL.sub(lambda literal: literal.group()[0] * 2, outside)
        lines.append(ScannedLine(number, raw, comment, strings, code))

    return lines


def split_c_line(raw: str, in_block_comment: bool) -> tuple[str | None, str, bool]:
    """Returns a line's comment text, its text outside comments, and whether a block comment is open at its end."""
    comments = []
    outside = []
    index = 0

    while index < len(raw):
        if in_block_comment:
            body, closer, _ = raw[index:].partition("*/")
            comments.append(strip_block_marker(body))
            in_block_comment = not closer
            index += len(body) + len(closer)
        elif raw.startswith("//", index):
            comments.append(raw[index + 2:])
            break
        elif raw.startswith("/*", index):
            in_block_comment = True
            index += 2
        elif raw[index] in "\"'":
            literal = STRING_LITERAL.match(raw, index).group()
            outside.append(literal)
            index += len(literal)
        else:
            outside.append(raw[index])
            index += 1

    comment = " ".join(comments) if comments else None

    return comment, "".join(outside), in_block_comment


def strip_block_marker(text: str) -> str:
    """Drops the `*` that opens a line of a block comment, leaving the spaces after it.

    A line without one loses its leading spaces, since it has no marker to indent past.
    """
    stripped = text.lstrip()
    return stripped[1:] if stripped.startswith("*") else stripped


def split_hash_line(raw: str) -> tuple[str | None, str]:
    """Returns the comment text of a shell or Makefile line and the text before it."""
    index = 0

    while index < len(raw):
        if raw[index] in "\"'":
            index += len(STRING_LITERAL.match(raw, index).group())
            continue

        # Only a `#` that starts a word opens a comment, so the shell's `$#` and `${#name}` stay code
        starts_word = index == 0 or raw[index - 1].isspace()
        if raw[index] == "#" and starts_word:
            return raw[index + 1:], raw[:index]

        index += 1

    return None, raw


def format_message(rule: Rule, text: str) -> str:
    """Fills a rule's message with the quoted text that broke it, or a lone non-ASCII character's code point."""
    if len(text) == 1 and not text.isascii():
        shown = f"U+{ord(text):04X} ({unicodedata.name(text, 'unnamed').lower()})"
    else:
        shown = f"'{text}'"

    return rule.message.format(shown)


def find_code_examples(lines: list[ScannedLine]) -> set[int]:
    """Returns the numbers of the comment lines that hold code examples.

    A comment line indented two or more spaces past its marker is a code example, unless it continues a list item
    or a doxygen tag other than `@code`, or continues such a continuation.
    """
    examples = set()
    continues_item = False

    for line in lines:
        comment = line.comment or ""
        is_indented = comment.startswith("  ")
        if not comment.strip():
            continues_item = False
        elif not is_indented:
            continues_item = bool(TAG_OR_LIST_ITEM.match(comment.lstrip()))
        elif not continues_item:
            examples.add(line.number)

    return examples


def run_line_checks(path: str, lines: list[ScannedLine], added: set[int]) -> list[Finding]:
    """Runs every line check over the added lines of a file."""
    findings = []
    examples = find_code_examples(lines)

    for line in lines:
        if line.number not in added:
            continue

        comment = line.comment or ""
        prose = "" if line.number in examples else BACKTICK_SPAN.sub(BACKTICK_WORD, comment).strip()
        parts = {
            LinePart.OUTSIDE_COMMENTS: f"{line.code} {line.strings}",
            LinePart.COMMENT: comment,
            LinePart.PROSE: prose,
        }

        for check in LINE_CHECKS:
            match = check.pattern.search(parts[check.part])
            if match:
                findings.append(Finding(path, line.number, check.rule, format_message(check.rule, match.group())))

    return findings


def check_stanzas(path: str, lines: list[ScannedLine], added: set[int]) -> list[Finding]:
    """Checks the stanza rules on the added lines of a C or C++ file."""
    return StanzaReader(path, lines, added).read()


def find_first_added(numbers: Iterable[int], added: set[int]) -> int | None:
    """Returns the first of the line numbers that the change adds, or `None` when it adds none of them.

    A finding is reported on that line, so its annotation shows in a pull request's diff.
    """
    return next((number for number in numbers if number in added), None)


def check_boundaries(path: str, lines: list[ScannedLine], added: set[int]) -> list[Finding]:
    """Flags, for each namespace of another subsystem a kernel header starts to use, the first added line using it.

    A header is its subsystem's interface to every file that includes it. When its declarations name another
    subsystem's types, every one of those files depends on that subsystem too, while the interface should use its own
    subsystem's types and convert the other's once, in its implementation.

    Raises `LintError` when git fails while the kernel's layout is read.
    """
    if not path.startswith("kernel/"):
        return []

    return find_foreign_namespaces(path, lines, added, read_kernel_layout())


def find_foreign_namespaces(
    path: str,
    lines: list[ScannedLine],
    added: set[int],
    layout: KernelLayout,
) -> list[Finding]:
    """Reports, for each namespace of another subsystem a kernel header names, the first added line naming it.

    A namespace an unchanged line of the header already names is a dependency the header has, and is not reported.
    Nor are the namespaces of the `SHARED_SUBSYSTEMS`, or those of `arch` in a header under `kernel/arch/`. Headers of
    the `UNCHECKED_SUBSYSTEMS` report nothing.
    """
    subsystem = find_subsystem(path, layout.subsystems)
    if subsystem is None or subsystem in UNCHECKED_SUBSYSTEMS:
        return []

    is_under_arch = path.startswith("kernel/arch/")
    named_before = {name for line in lines if line.number not in added for name in find_named_namespaces(line.code)}
    findings = []

    for line in lines:
        if line.number not in added:
            continue

        for name in find_named_namespaces(line.code):
            home = layout.namespace_homes.get(name)
            is_foreign = home not in (None, subsystem) and home not in SHARED_SUBSYSTEMS
            is_own_arch = home == "arch" and is_under_arch
            if not is_foreign or is_own_arch or name in named_before:
                continue

            named_before.add(name)
            message = Rule.FOREIGN_NAMESPACE.message.format(f"`{subsystem}/`", name, f"`{home}/`")
            findings.append(Finding(path, line.number, Rule.FOREIGN_NAMESPACE, message))

    return findings


def find_named_namespaces(code: str) -> list[str]:
    """Returns the namespaces a line's code names, in a qualified name or after `namespace`."""
    return NAMESPACE_USE.findall(code) + NAMESPACE_NAMED.findall(code)


@functools.cache
def read_kernel_layout() -> KernelLayout:
    """Reads the kernel's layout from its C and C++ files, tracked or untracked.

    Raises `LintError` when git fails.
    """
    listing = run_git("ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", "kernel")
    paths = [path for path in listing.split("\0") if find_file_type(path) in C_CODE_FILE_TYPES]
    texts = {path: Path(path).read_text(encoding="utf-8", errors="replace") for path in paths if os.path.isfile(path)}

    return build_kernel_layout(texts)


def build_kernel_layout(texts: dict[str, str]) -> KernelLayout:
    """Finds the kernel's subsystems in the paths of its files, and the subsystem each namespace belongs to.

    A namespace belongs to the subsystem of the same name, as the guide's "namespace = directory = module name" asks.
    A namespace without one belongs to the subsystem whose files open the most blocks defining it outside
    `kernel/arch/` and `tests/`, else to the subsystem its files under `kernel/arch/` all belong to, else to `arch`
    when they belong to several, else to `tests`. A namespace block that only forward-declares types defines nothing.
    """
    subsystems = frozenset(path.split("/")[1] for path in texts if path.count("/") >= 2)
    definers = defaultdict(Counter)
    arch_definers = defaultdict(set)

    for path, text in texts.items():
        subsystem = find_subsystem(path, subsystems)
        is_under_arch = path.startswith("kernel/arch/")
        for name in find_defined_namespaces(path, text):
            if is_under_arch:
                arch_definers[name].add(subsystem)
            else:
                definers[name][subsystem] += 1

    namespace_homes = {name: name for name in subsystems}

    for name in definers.keys() | arch_definers.keys():
        if name in subsystems:
            continue

        others = Counter({definer: count for definer, count in definers[name].items() if definer != "tests"})
        arch_homes = arch_definers[name]
        if others:
            namespace_homes[name] = others.most_common(1)[0][0]
        elif arch_homes:
            namespace_homes[name] = next(iter(arch_homes)) if len(arch_homes) == 1 else "arch"
        else:
            namespace_homes[name] = "tests"

    return KernelLayout(subsystems, namespace_homes)


def find_defined_namespaces(path: str, text: str) -> list[str]:
    """Returns the top-level namespaces a file opens to define something, once per namespace block."""
    code = "\n".join(line.code for line in scan_file(path, text) if not line.code.lstrip().startswith("#"))
    names = []
    index = 0

    while index < len(code):
        block = NAMESPACE_BLOCK.match(code, index)
        if not block:
            index += 1
            continue

        end = find_closing_brace(code, block.end())
        body = code[block.end():end]
        if not all(FORWARD_DECLARATION.fullmatch(part) for part in body.split(";")):
            names.append(block.group(1))

        index = end + 1

    return names


def find_closing_brace(code: str, start: int) -> int:
    """Returns the index of the `}` that closes a block whose body starts at `start`, or the end of the code."""
    depth = 1

    for index in range(start, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1

        if depth == 0:
            return index

    return len(code)


def find_subsystem(path: str, subsystems: frozenset[str]) -> str | None:
    """Returns the kernel subsystem a file belongs to, or `None` outside the kernel's subsystems.

    A subsystem is a directory under `kernel/`, and a directory of the same name in an arch tree belongs to it, as
    the arch-layout rule's mirror naming requires. Files directly under an arch tree, and in an arch tree's
    directories that mirror no subsystem, such as x86_64's `defs/`, belong to `arch`.
    """
    parts = path.split("/")
    if parts[0] != "kernel" or len(parts) < 3:
        return None

    if parts[1] != "arch":
        return parts[1]

    is_mirrored = len(parts) > 4 and parts[3] in subsystems
    return parts[3] if is_mirrored else "arch"


# Defined after the check functions it names, since Python needs a function to exist before a table can hold it
CHECK_FAMILIES = (
    CheckFamily(C_COMMENT_FILE_TYPES | HASH_COMMENT_FILE_TYPES, run_line_checks),
    CheckFamily(C_CODE_FILE_TYPES, check_stanzas),
    CheckFamily(HEADER_FILE_TYPES, check_boundaries),
)


def find_check_families(path: str) -> list[CheckFamily]:
    """Returns the check families that cover the file's type."""
    return [family for family in CHECK_FAMILIES if find_file_type(path) in family.file_types]


def check_file(path: str, text: str, added: set[int] | None) -> list[Finding]:
    """Runs the check families for the file's type over its added lines, or over every line when `added` is `None`."""
    lines = scan_file(path, text)
    numbers = {line.number for line in lines} if added is None else added
    findings = []

    for family in find_check_families(path):
        findings.extend(family.check(path, lines, numbers))

    return findings


def run_git(*arguments: str, stdin: str = "") -> str:
    """Runs git in the current directory and returns its output.

    Raises `LintError` when git cannot run or exits with an error.
    """
    command = ["git", *arguments]

    try:
        result = subprocess.run(command, input=stdin, capture_output=True, encoding="utf-8", errors="replace")
    except OSError as error:
        raise LintError(f"cannot run git: {error.strerror}") from error

    if result.returncode != 0:
        raise LintError(f"{' '.join(command)} failed: {result.stderr.strip()}")

    return result.stdout


def read_style_globs() -> list[str]:
    """Returns the globs in the style guide's front matter, which name the files its rules cover.

    Raises `LintError` when the guide cannot be read or has no globs line, when a glob matches no tracked file, as a
    quoted or list-style globs line does, or when git fails.
    """
    try:
        text = Path(STYLE_GUIDE).read_text(encoding="utf-8")
    except OSError as error:
        raise LintError(f"cannot read {STYLE_GUIDE}: {error.strerror}") from error

    front_matter = text.split("\n---", 1)[0]
    match = GLOBS_LINE.search(front_matter)
    if not match:
        raise LintError(f"{STYLE_GUIDE} has no globs line in its front matter, so the files to check are unknown")

    globs = [glob.strip() for glob in match.group(1).split(",")]

    for glob in globs:
        if not run_git("ls-files", "--", f":(glob){glob}"):
            raise LintError(f"the glob {glob!r} in {STYLE_GUIDE} matches no tracked file")

    return globs


def collect_added_lines(base: str, globs: list[str]) -> dict[str, set[int] | None]:
    """Maps each file the change touches within the globs to the lines it adds, or to `None` when it is untracked.

    Raises `LintError` when git fails or its diff cannot be parsed: a header names a path git had to quote, a hunk
    does not match its header, or a changed line falls outside every hunk.
    """
    pathspecs = [f":(glob){glob}" for glob in globs]
    merge_base = run_git("merge-base", base, "HEAD").strip()
    diff = run_git("-c", "core.quotePath=false", "diff", *DIFF_OPTIONS, merge_base, "--", *pathspecs)
    listing = run_git("ls-files", "-z", "--others", "--exclude-standard", "--", *pathspecs)
    untracked = [path for path in listing.split("\0") if path]

    return {**parse_added_lines(diff), **dict.fromkeys(untracked)}


def parse_added_lines(diff: str) -> dict[str, set[int]]:
    """Maps each file in a diff to the line numbers its hunks add.

    Raises `LintError` when a header names a path git had to quote, a hunk does not match its header, or a changed
    line falls outside every hunk.
    """
    added = {}
    path = None
    lines = iter(diff.split("\n"))

    for line in lines:
        is_changed_line = line.startswith((" ", "+", "-")) and not line.startswith(("--- ", "+++ "))
        if line.startswith("+++ "):
            path = read_new_path(line)
        elif line.startswith("@@ "):
            numbers = read_hunk(line, lines)
            if path is not None:
                added.setdefault(path, set()).update(numbers)
        elif is_changed_line:
            raise LintError(f"the diff of {path} has a changed line outside every hunk: {line!r}")

    return added


def read_new_path(header: str) -> str | None:
    """Returns the path a `+++` header names, or `None` for a deleted file.

    Raises `LintError` for a path git had to quote, as it does for a name holding a double quote or a backslash.
    """
    # Git ends a path that holds a space with a tab
    name = header[len("+++ "):].rstrip("\t")
    if name == "/dev/null":
        return None

    if not name.startswith("b/"):
        raise LintError(f"cannot read the path in the diff header {header!r}, git quoted it")

    return name[len("b/"):]


def read_hunk(header: str, lines: Iterator[str]) -> set[int]:
    """Reads the lines of one hunk and returns the line numbers it adds, counting context lines on both sides.

    Raises `LintError` when the hunk holds more or fewer lines than its header counts.
    """
    header_fields = HUNK_HEADER.match(header)
    old_left = int(header_fields.group(1) or "1")
    number = int(header_fields.group(2))
    new_left = int(header_fields.group(3) or "1")
    added = set()

    while old_left > 0 or new_left > 0:
        line = next(lines, "")
        if line.startswith("+"):
            added.add(number)
            number += 1
            new_left -= 1
        elif line.startswith("-"):
            old_left -= 1
        elif line.startswith(" "):
            number += 1
            old_left -= 1
            new_left -= 1
        elif not line.startswith("\\"):
            break

    if old_left or new_left:
        raise LintError(f"the hunk {header!r} holds more or fewer lines than its header counts")

    return added


def find_vendored_paths(paths: list[str]) -> set[str]:
    """Returns the paths `.gitattributes` marks `linguist-vendored`, which hold third-party code.

    Raises `LintError` when git fails.
    """
    output = run_git("check-attr", "-z", "--stdin", "linguist-vendored", stdin="".join(f"{path}\0" for path in paths))
    fields = output.split("\0")
    # Each path comes back as three fields: the path, the attribute, and its value
    return {path for path, value in zip(fields[0::3], fields[2::3]) if value == "set"}


def read_changed_file(path: str) -> str:
    """Returns the text of a file the change touches.

    Raises `LintError` when the file cannot be read, as a broken symlink cannot.
    """
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise LintError(f"cannot read {path}: {error.strerror}") from error


def report_findings(findings: list[Finding], checked_count: int) -> None:
    """Prints each finding and a summary, and annotates the pull request when running in GitHub Actions."""
    in_github_actions = os.environ.get("GITHUB_ACTIONS") == "true"
    error_count = sum(finding.rule.is_error for finding in findings)
    warning_count = len(findings) - error_count

    for finding in findings:
        severity = "error" if finding.rule.is_error else "warning"
        print(f"  {severity} {finding.path}:{finding.number}: {finding.message}")

        if in_github_actions:
            print(f"::{severity} file={finding.path},line={finding.number}::{finding.message}")

    print(f"style_lint: {checked_count} file(s) checked, {error_count} error(s), {warning_count} warning(s)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", default="origin/master", help="check the change since the merge base with BASE")
    arguments = parser.parse_args()

    try:
        os.chdir(run_git("rev-parse", "--show-toplevel").strip())
        changes = collect_added_lines(arguments.base, read_style_globs())
        vendored = find_vendored_paths(list(changes))
        checked = [path for path in sorted(changes) if path not in vendored and find_check_families(path)]
        texts = {path: read_changed_file(path) for path in checked}
        findings = [finding for path in checked for finding in check_file(path, texts[path], changes[path])]
    except LintError as error:
        print(f"style_lint: {error}", file=sys.stderr)
        return 2

    report_findings(findings, len(checked))

    return 1 if any(finding.rule.is_error for finding in findings) else 0


if __name__ == "__main__":
    sys.exit(main())
