#!/usr/bin/env python3
"""Checks the lines a change adds against the StelluxOS style guide, `.cursor/rules/style.mdc`.

Comments, code and strings must be ASCII only, as the guide's language rules require. Comments must also follow its
rules on dashes, semicolons, banned words and wording that dates them, and must not draw decorative separators. A few
conventions the guide leaves unwritten, such as keeping `TODO` markers out of comments, are checked as well.

The files checked are the paths the globs line of the guide's front matter matches, minus the paths `.gitattributes`
marks `linguist-vendored`, which hold third-party code. Files ending in `.c`, `.cpp`, `.h`, `.hpp`, `.S`, `.ld` or
`.inc` are read with C-style comments, and files ending in `.sh` or `.mk` and files named `Makefile` with hash
comments. Other files the globs match are not read. A change is every commit since the merge base of `HEAD` and the
base ref, `origin/master` unless `--base` names another, plus the uncommitted edits. Only the lines it adds or edits
are checked, so untouched code keeps the style it has. Untracked files are checked whole.

Text in backticks names code, and a comment line indented two or more spaces past its marker is a code example
unless it continues a list item or a doxygen tag other than `@code`, so an example under a list item or a tag
needs a lead-in line such as "Usage:". Of the comment checks, only those for non-ASCII characters and typographic
dashes read either.

Breaking a rule the guide writes down is an error and fails the run. A match that may be innocent, such as a phrase
that may date the comment, is a warning, and so is a break of an unwritten convention. In GitHub Actions every
finding is also printed as an annotation, so errors and warnings both appear on the pull request.

Usage:
    scripts/style_lint.py                checks the change since the merge base with origin/master
    scripts/style_lint.py --base <ref>   checks the change since the merge base with <ref>

Exits with 0 when no error is found and 1 when one is. Exits with 2 when git fails, the guide cannot be read, its
globs line is missing or holds a glob that matches no tracked file, the diff cannot be parsed, or a changed file
cannot be read.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import unicodedata
from enum import Enum, auto
from pathlib import Path
from typing import Callable, Iterator, NamedTuple

STYLE_GUIDE = ".cursor/rules/style.mdc"
GLOBS_LINE = re.compile(r"^globs:[ \t]*(\S.*)$", re.MULTILINE)

C_COMMENT_FILE_TYPES = frozenset({".c", ".cpp", ".h", ".hpp", ".S", ".ld", ".inc"})
HASH_COMMENT_FILE_TYPES = frozenset({".sh", ".mk", "Makefile"})

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


class LintError(Exception):
    """A failure that stops the run before any file is checked, such as git failing."""


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


# Defined after the check functions it names, since Python needs a function to exist before a table can hold it
CHECK_FAMILIES = (
    CheckFamily(C_COMMENT_FILE_TYPES | HASH_COMMENT_FILE_TYPES, run_line_checks),
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
    except LintError as error:
        print(f"style_lint: {error}", file=sys.stderr)
        return 2

    findings = []

    for path in checked:
        findings.extend(check_file(path, texts[path], changes[path]))

    report_findings(findings, len(checked))

    return 1 if any(finding.rule.is_error for finding in findings) else 0


if __name__ == "__main__":
    sys.exit(main())
