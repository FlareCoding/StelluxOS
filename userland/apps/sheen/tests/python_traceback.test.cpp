/* Host unit tests for Sheen's shared terminal escape/path helpers and Python 3 traceback parser. */

#include "extensions/python/python_traceback.h"
#include "extensions/workspace_paths.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

static constexpr uint32_t MAX_FAILURE_REPORTS = 12;

static uint64_t g_checks = 0;
static uint64_t g_failures = 0;

static void check(bool holds, const char* description) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_FAILURE_REPORTS) {
        std::printf("  FAIL: %s\n", description);
    }
}

static std::string create_temp_workspace() {
    const char* tmp_env = std::getenv("TMPDIR");
    std::string base = tmp_env != nullptr && tmp_env[0] != '\0' ? std::string(tmp_env) : std::string("/tmp");
    if (!base.empty() && base.back() == '/') {
        base.pop_back();
    }

    std::string pattern = base + "/sheen-python-test-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');

    char* created = ::mkdtemp(writable.data());
    check(created != nullptr, "mkdtemp creates temporary workspace");
    if (created == nullptr) {
        return {};
    }

    char canonical[PATH_MAX];
    if (::realpath(created, canonical) != nullptr) {
        return canonical;
    }

    return created;
}

static void write_text_file(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

static void test_streaming_escape_filter_and_paths() {
    std::string raw_sgr = "\x1b[01m\x1b[Kmain.py:12:\x1b[m\x1b[K \x1b[01;31mError\x1b[m\r\n";
    check(sheen::strip_terminal_escapes(raw_sgr) == "main.py:12: Error\n",
          "strip_terminal_escapes removes CSI SGR sequences and CR");

    std::string raw_osc = "\x1b]8;;file:///work/a.py\x07" "a.py\x1b]8;;\x1b\\:4\r\n";
    check(sheen::strip_terminal_escapes(raw_osc) == "a.py:4\n",
          "strip_terminal_escapes removes BEL and ST terminated OSC hyperlinks");

    sheen::terminal_escape_filter split_filter;
    std::string split_out;
    for (char ch : raw_sgr) {
        split_out += split_filter.feed(std::string_view(&ch, 1));
    }

    check(split_out == "main.py:12: Error\n",
          "terminal_escape_filter strips CSI escapes split byte-by-byte across read chunks");

    split_filter.reset();
    std::string osc_out;
    for (size_t i = 0; i < raw_osc.size(); i += 2) {
        osc_out += split_filter.feed(std::string_view(raw_osc).substr(i, 2));
    }

    check(osc_out == "a.py:4\n",
          "terminal_escape_filter strips OSC escapes split across two-byte chunks");

    check(sheen::resolve_workspace_file_path("/work/proj", "src/main.py") == "/work/proj/src/main.py",
          "relative path inside synthetic workspace resolves cleanly");
    check(sheen::resolve_workspace_file_path("/work/proj/", "./sub/../app.py") == "/work/proj/app.py",
          "dot segments inside workspace normalize cleanly");

    check(sheen::resolve_workspace_file_path("/work/proj", "/work/proj").empty(),
          "workspace directory itself is not accepted as a source file");
    check(sheen::resolve_workspace_file_path("/work/proj", "../outside.py").empty(),
          "parent traversal outside workspace is rejected");
    check(sheen::resolve_workspace_file_path("/work/proj", "/work/proj_sibling/a.py").empty(),
          "prefix-sharing sibling directory outside workspace is rejected");

    check(sheen::resolve_workspace_file_path("/work/proj", "/usr/lib/python3.12/json/__init__.py").empty(),
          "stdlib path outside workspace is rejected");
    check(sheen::resolve_workspace_file_path("/work/proj", "<stdin>").empty(),
          "synthetic angle-bracket pseudo-file is rejected");
    check(sheen::relative_workspace_display_path("/work/proj", "/work/proj/src/calc.py") == "src/calc.py",
          "relative_workspace_display_path strips workspace prefix");
}

static void test_symlink_containment_and_existing_files() {
    std::string ws = create_temp_workspace();
    if (ws.empty()) {
        return;
    }

    std::filesystem::path root(ws);
    write_text_file(root / "inside.py", "x = 1\n");

    std::string outside_dir = create_temp_workspace();
    std::filesystem::path outside_file = std::filesystem::path(outside_dir) / "secret.py";
    write_text_file(outside_file, "secret = 42\n");

    std::error_code ec;
    std::filesystem::create_symlink(outside_file, root / "escape.py", ec);

    check(sheen::resolve_workspace_file_path(ws, "inside.py", true) == (root / "inside.py").generic_string(),
          "existing file inside real workspace resolves");
    check(sheen::resolve_workspace_file_path(ws, "missing.py", true).empty(),
          "missing file fails when require_existing_file is true");
    check(!sheen::resolve_workspace_file_path(ws, "missing.py", false).empty(),
          "missing file resolves lexically when require_existing_file is false");

    if (!ec) {
        check(sheen::resolve_workspace_file_path(ws, "escape.py", true).empty(),
              "symlink pointing outside workspace root is rejected");
    }

    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(outside_dir, ec);
}

static void test_python_runtime_tracebacks_and_live_repl_refresh() {
    const std::string first_trace =
        "Loading dataset...\r\n"
        "Traceback (most recent call last):\r\n"
        "  File \"/work/demo/main.py\", line 19, in <module>\r\n"
        "    run_job()\r\n"
        "  File \"src/worker.py\", line 31, in run_job\r\n"
        "    parsed = json.loads(raw_payload)\r\n"
        "             ^^^^^^^^^^^^^^^^^^^^^^^\r\n"
        "  File \"/usr/lib/python3.12/json/__init__.py\", line 346, in loads\r\n"
        "    return _default_decoder.decode(s)\r\n"
        "json.decoder.JSONDecodeError: Expecting value: line 1 column 1 (char 0)\r\n";

    sheen::python_traceback_parser parser("/work/demo");
    bool emitted = parser.feed(first_trace);

    check(emitted, "runtime traceback feed reports completed diagnostic immediately");
    check(parser.error_count() == 1, "runtime traceback records one error diagnostic");
    check(parser.frames().size() == 3, "all three traceback frames are recorded");

    if (parser.diagnostics().size() == 1) {
        const sheen::script_diagnostic& diag = parser.diagnostics()[0];
        check(diag.file_path == "/work/demo/src/worker.py",
              "diagnostic attaches to innermost workspace frame rather than stdlib frame");
        check(diag.line == 31 && diag.column == 10, "PEP 657 line and caret column are captured");
        check(diag.message == "json.decoder.JSONDecodeError: Expecting value: line 1 column 1 (char 0)",
              "qualified exception message is preserved whole");
    }

    check(parser.take_pending_jump_lines() == "  at main.py:19 in <module>\n  at src/worker.py:31:10 in run_job\n",
          "take_pending_jump_lines emits clickable workspace links on completion");
    check(parser.take_pending_jump_lines().empty(),
          "second take_pending_jump_lines call returns empty until next traceback");

    const std::string second_repl_trace =
        ">>> reload_and_retry()\r\n"
        "Traceback (most recent call last):\r\n"
        "  File \"/work/demo/main.py\", line 7, in reload_and_retry\r\n"
        "    raise KeyError('port')\r\n"
        "KeyError: 'port'\r\n";

    check(parser.feed(second_repl_trace), "subsequent REPL traceback updates diagnostics live");
    check(parser.diagnostics().size() == 1 && parser.diagnostics()[0].line == 7,
          "fresh non-chained REPL traceback replaces stale previous diagnostic");
    check(parser.take_pending_jump_lines() == "  at main.py:7 in reload_and_retry\n",
          "take_pending_jump_lines emits only newly completed REPL traceback frames");

    parser.reset();
    check(parser.diagnostics().empty() && parser.frames().empty() && parser.error_count() == 0,
          "python_traceback_parser::reset clears diagnostics and frames");
}

static void test_python_syntax_errors_and_indented_disk_columns() {
    std::string ws = create_temp_workspace();
    if (ws.empty()) {
        return;
    }

    std::filesystem::path root(ws);
    write_text_file(root / "broken.py", "def compute():\n        total = 10 +\n");
    write_text_file(root / "div.py", "def divide():\n        return 100 / 0\ndivide()\n");

    sheen::python_traceback_parser syn_parser(ws, true);
    syn_parser.feed(
        "  File \"" + (root / "broken.py").generic_string() + "\", line 2\n"
        "    total = 10 +\n"
        "                ^\n"
        "SyntaxError: invalid syntax\n");
    syn_parser.finish();

    check(syn_parser.diagnostics().size() == 1,
          "standalone SyntaxError without Traceback header emits one diagnostic");
    if (syn_parser.diagnostics().size() == 1) {
        check(syn_parser.diagnostics()[0].file_path == (root / "broken.py").generic_string(),
              "SyntaxError file path matches");
        check(syn_parser.diagnostics()[0].line == 2 && syn_parser.diagnostics()[0].column == 21,
              "SyntaxError caret column restores stripped leading file indentation");
    }

    sheen::python_traceback_parser rt_parser(ws, true);
    rt_parser.feed(
        "Traceback (most recent call last):\n"
        "  File \"" + (root / "div.py").generic_string() + "\", line 3, in <module>\n"
        "    divide()\n"
        "  File \"" + (root / "div.py").generic_string() + "\", line 2, in divide\n"
        "    return 100 / 0\n"
        "           ~~~~^~~\n"
        "ZeroDivisionError: division by zero\n");
    rt_parser.finish();

    check(rt_parser.diagnostics().size() == 1 && rt_parser.diagnostics()[0].column == 20,
          "PEP 657 tilde-caret locates exact slash operator column on indented file line");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

static void test_python_chained_streaming_and_discovery() {
    const std::string chained =
        "File \"main.py\", line 1\n"
        "  File \"/work/app/bad.py\", line 0, in zero_line\n"
        "Traceback (most recent call last):\n"
        "  File \"/work/app/config.py\", line 9, in load_port\n"
        "    return int(raw)\n"
        "ValueError: invalid literal for int() with base 10: 'abc'\n"
        "\n"
        "During handling of the above exception, another exception occurred:\n"
        "\n"
        "Traceback (most recent call last):\n"
        "  File \"/work/app/main.py\", line 15, in boot\n"
        "    raise AssertionError\n"
        "AssertionError";

    sheen::python_traceback_parser parser("/work/app");
    for (char ch : chained) {
        parser.feed(std::string_view(&ch, 1));
    }

    check(parser.diagnostics().size() == 1, "first chained exception finalized before unterminated tail");
    check(parser.finish(), "finish flushes unterminated final exception line");
    check(parser.diagnostics().size() == 2, "chained continuation keeps both exception diagnostics");

    sheen::python_traceback_parser warn_parser("/work/app");
    warn_parser.feed(
        "  File \"/work/app/caret.py\", line 1\n"
        "    ^\n"
        "    ^\n"
        "SyntaxWarning: suspicious token\n"
        "  File \"/work/app/eof.py\", line 3\n"
        "\n"
        "      ^\n"
        "IndentationError: expected an indented block\n");
    warn_parser.finish();

    check(warn_parser.diagnostics().size() == 1 && warn_parser.diagnostics()[0].line == 3
              && warn_parser.diagnostics()[0].column == 3,
          "second standalone syntax error replaces earlier warning diagnostic with column 3");

    std::string ws = create_temp_workspace();
    if (ws.empty()) {
        return;
    }

    std::filesystem::path root(ws);
    write_text_file(root / "zebra.py", "print(1)\n");
    write_text_file(root / "app.py", "print(2)\n");
    write_text_file(root / "main.py", "print(3)\n");

    write_text_file(root / "tools" / "gen.py", "print(4)\n");
    write_text_file(root / ".venv" / "ignored.py", "print(5)\n");
    write_text_file(root / "__pycache__" / "cached.py", "print(6)\n");

    check(sheen::is_python_source_path("SCRIPT.PY") && !sheen::is_python_source_path("script.pyw"),
          "is_python_source_path matches .py case-insensitively");
    check(sheen::discover_python_targets(ws) == std::vector<std::string>{
              (root / "main.py").generic_string(),
              (root / "app.py").generic_string(),
              (root / "zebra.py").generic_string(),
              (root / "tools" / "gen.py").generic_string(),
          },
          "discover_python_targets prioritizes main.py then app.py and skips .venv and __pycache__");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

int main() {
    test_streaming_escape_filter_and_paths();
    test_symlink_containment_and_existing_files();
    test_python_runtime_tracebacks_and_live_repl_refresh();
    test_python_syntax_errors_and_indented_disk_columns();
    test_python_chained_streaming_and_discovery();

    if (g_failures != 0) {
        std::printf("python_traceback: %llu of %llu checks failed\n",
                    static_cast<unsigned long long>(g_failures),
                    static_cast<unsigned long long>(g_checks));
        return 1;
    }

    std::printf("python_traceback: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
