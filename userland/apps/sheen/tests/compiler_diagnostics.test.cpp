/* Host unit tests for Sheen's C and C++ compiler/linker diagnostic parser, target discovery, and build plans. */

#include "extensions/c_run/compiler_diagnostics.h"

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
static constexpr size_t EXPECTED_BUILD_PATH_LEN = 33;

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

    std::string pattern = base + "/sheen-c-run-test-XXXXXX";
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

static void test_compiler_diagnostics_gcc_clang_tcc_and_ld() {
    const std::string compiler_stream =
        "In file included from main.cpp:2:\r\n"
        "\x1b[01m\x1b[Ksrc/math.cpp:18:13:\x1b[m\x1b[K \x1b[01;35m\x1b[Kwarning:\x1b[m\x1b[K "
        "unused variable 'scale' [-Wunused-variable]\r\n"
        "   18 |     int scale = 2;\r\n"
        "      |         ^~~~~\r\n"
        "/usr/gcc/include/stdio.h:55:1: note: declared here\r\n"
        "src/math.cpp:24:9: error: 'missing_val' was not declared in this scope\r\n"
        "src/math.cpp:24:9: note: suggested alternative: 'existing_val'\r\n"
        "legacy.c:7: error: ';' expected (got \"return\")\r\n"
        "include/missing.h:1:10: fatal error: board/pins.h: No such file or directory\r\n"
        "/usr/bin/ld: /work/cproj/src/main.cpp:41:(.text+0x28): undefined reference to `render_frame()'\r\n"
        "/usr/gcc/bin/ld: /work/cproj/src/dup.c:8: multiple definition of `helper'\r\n"
        "src/math.cpp:0:1: error: zero line must be ignored\r\n"
        "cc1plus: error: unrecognized command-line option '-fbad'";

    sheen::compiler_output_parser parser("/work/cproj");
    for (size_t i = 0; i < compiler_stream.size(); i += 7) {
        parser.feed(std::string_view(compiler_stream).substr(i, 7));
    }

    parser.finish();

    check(parser.diagnostics().size() == 6, "compiler parser emits six workspace diagnostics and skips notes");
    check(parser.error_count() == 5, "compiler parser counts five errors");
    check(parser.warning_count() == 1, "compiler parser counts one warning");

    if (parser.diagnostics().size() == 6) {
        check(parser.diagnostics()[0] == sheen::script_diagnostic{
                  "/work/cproj/src/math.cpp", 18, 13, sheen::script_diagnostic_severity::WARNING,
                  "unused variable 'scale' [-Wunused-variable]" },
              "ANSI-colored GCC warning parses file, line, col, and message");
        check(parser.diagnostics()[1] == sheen::script_diagnostic{
                  "/work/cproj/src/math.cpp", 24, 9, sheen::script_diagnostic_severity::ERROR,
                  "'missing_val' was not declared in this scope" },
              "GCC error parses file, line, col, and message");
        check(parser.diagnostics()[2] == sheen::script_diagnostic{
                  "/work/cproj/legacy.c", 7, 0, sheen::script_diagnostic_severity::ERROR,
                  "';' expected (got \"return\")" },
              "TCC column-less error sets column to 0");
        check(parser.diagnostics()[3] == sheen::script_diagnostic{
                  "/work/cproj/include/missing.h", 1, 10, sheen::script_diagnostic_severity::ERROR,
                  "board/pins.h: No such file or directory" },
              "fatal error preserves interior colon inside header message");
        check(parser.diagnostics()[4] == sheen::script_diagnostic{
                  "/work/cproj/src/main.cpp", 41, 0, sheen::script_diagnostic_severity::ERROR,
                  "undefined reference to `render_frame()'" },
              "GNU ld prefixed undefined reference strips ld prefix and section offset");
        check(parser.diagnostics()[5] == sheen::script_diagnostic{
                  "/work/cproj/src/dup.c", 8, 0, sheen::script_diagnostic_severity::ERROR,
                  "multiple definition of `helper'" },
              "GNU ld multiple definition error parses cleanly");
    }

    parser.feed("src/math.cpp:24:9: error: 'missing_val' was not declared in this scope\n");
    check(parser.diagnostics().size() == 6, "duplicate compiler diagnostic is suppressed");

    parser.reset();
    check(parser.diagnostics().empty() && parser.error_count() == 0 && parser.warning_count() == 0,
          "compiler_output_parser::reset clears accumulated state");
}

static void test_source_classification_and_build_paths() {
    check(sheen::is_cpp_source_path("engine.CPP") && sheen::is_cpp_source_path("math.cc")
              && sheen::is_cpp_source_path("core.cxx") && !sheen::is_cpp_source_path("hello.c"),
          "is_cpp_source_path classifies C++ suffixes only");
    check(sheen::is_c_or_cpp_source_path("hello.C") && sheen::is_c_or_cpp_source_path("main.cpp")
              && !sheen::is_c_or_cpp_source_path("Makefile") && !sheen::is_c_or_cpp_source_path("header.h"),
          "is_c_or_cpp_source_path accepts C and C++ translation units only");

    std::string build_a = sheen::compute_build_directory_path("/work/demo");
    std::string build_a_slash = sheen::compute_build_directory_path("/work/demo/");
    std::string build_b = sheen::compute_build_directory_path("/work/other");
    check(build_a.starts_with("/tmp/sheen-build-") && build_a.size() == EXPECTED_BUILD_PATH_LEN,
          "compute_build_directory_path produces /tmp/sheen-build-<16-hex>");
    check(build_a == build_a_slash, "trailing slash produces identical build directory hash");
    check(build_a != build_b, "distinct workspaces produce distinct build directory hashes");

    check(sheen::compute_target_output_binary_path("/work/demo", "/work/demo/hello.c") == build_a + "/hello",
          "top-level source maps to /tmp/sheen-build-<hash>/<stem>");
    check(sheen::compute_target_output_binary_path("/work/demo", "/work/demo/src/hello.cpp") == build_a + "/src_hello",
          "nested source disambiguates output binary name with relative directory");
    check(sheen::compute_project_output_binary_path("/work/demo/") == build_a + "/demo",
          "project output binary takes sanitized workspace folder name");
}

static void test_compiler_build_plans_pure_and_mixed() {
    sheen::compiler_environment env{
        "/work/mixed",
        "/usr/gcc/bin/gcc",
        "/usr/gcc/bin/g++",
        true,
        true,
        true,
        true,
        true,
    };

    std::string build_dir = sheen::compute_build_directory_path("/work/mixed");

    sheen::compiler_build_plan pure_c = sheen::build_compiler_plan(
        env, { "/work/mixed/hello.c" }, false);
    check(pure_c.steps.size() == 1 && pure_c.object_files.empty(),
          "pure C target emits a single compile-and-link step");
    if (pure_c.steps.size() == 1) {
        check(pure_c.steps[0].program == "/usr/gcc/bin/gcc", "pure C step invokes C compiler");
        check(pure_c.steps[0].arguments == std::vector<std::string>{
                  "-std=gnu17", "-Wall", "-Wextra", "-g",
                  "-iquote", "/work/mixed", "-iquote", "/work/mixed/src",
                  "-I/work/mixed/include", "-idirafter", "/usr/include",
                  "hello.c", "/usr/lib/libstlx.a", "-lm", "-o", build_dir + "/hello" },
              "pure C step passes -std=gnu17, -iquote, -idirafter /usr/include, and libstlx.a");
    }

    sheen::compiler_build_plan pure_cxx = sheen::build_compiler_plan(
        env, { "/work/mixed/src/app.cpp" }, false);
    check(pure_cxx.steps.size() == 1 && pure_cxx.object_files.empty(),
          "pure C++ target emits a single compile-and-link step");
    if (pure_cxx.steps.size() == 1) {
        check(pure_cxx.steps[0].program == "/usr/gcc/bin/g++", "pure C++ step invokes C++ compiler");
        check(pure_cxx.steps[0].arguments.front() == "-std=c++20", "pure C++ step passes -std=c++20");
    }

    sheen::compiler_build_plan mixed = sheen::build_compiler_plan(
        env,
        { "/work/mixed/helper.c", "/work/mixed/src/codec.c", "/work/mixed/src/main.cpp" },
        true);

    check(mixed.steps.size() == 3 && mixed.object_files.size() == 2,
          "mixed C and C++ build compiles each .c file with C compiler before C++ compile-and-link");
    if (mixed.steps.size() == 3 && mixed.object_files.size() == 2) {
        check(mixed.object_files[0] != mixed.object_files[1],
              "mixed build assigns distinct temporary object paths to each C source");
        check(mixed.steps[0] == sheen::compiler_step{
                  "/usr/gcc/bin/gcc",
                  { "-std=gnu17", "-Wall", "-Wextra", "-g",
                    "-iquote", "/work/mixed", "-iquote", "/work/mixed/src",
                    "-I/work/mixed/include", "-idirafter", "/usr/include",
                    "-c", "helper.c", "-o", mixed.object_files[0] } },
              "mixed step 0 compiles helper.c as C17 object without C++ flags");
        check(mixed.steps[1] == sheen::compiler_step{
                  "/usr/gcc/bin/gcc",
                  { "-std=gnu17", "-Wall", "-Wextra", "-g",
                    "-iquote", "/work/mixed", "-iquote", "/work/mixed/src",
                    "-I/work/mixed/include", "-idirafter", "/usr/include",
                    "-c", "src/codec.c", "-o", mixed.object_files[1] } },
              "mixed step 1 compiles src/codec.c as C17 object");
        check(mixed.steps[2] == sheen::compiler_step{
                  "/usr/gcc/bin/g++",
                  { "-std=c++20", "-Wall", "-Wextra", "-g",
                    "-iquote", "/work/mixed", "-iquote", "/work/mixed/src",
                    "-I/work/mixed/include", "-idirafter", "/usr/include",
                    "src/main.cpp", mixed.object_files[0], mixed.object_files[1],
                    "/usr/lib/libstlx.a", "-lm", "-o", build_dir + "/mixed" } },
              "mixed link step compiles C++20 sources with g++ and links C object files before libstlx.a");
    }
}

static void test_c_run_target_and_project_source_discovery() {
    std::string ws = create_temp_workspace();
    if (ws.empty()) {
        return;
    }

    std::filesystem::path root(ws);
    write_text_file(root / "Makefile", "all:\n");
    write_text_file(root / "util.c", "int u(void) { return 1; }\n");
    write_text_file(root / "main.cpp", "int main() { return 0; }\n");
    write_text_file(root / "src" / "engine.cpp", "int e() { return 2; }\n");

    write_text_file(root / "tests" / "unit_main.cpp", "int main() { return 0; }\n");
    write_text_file(root / "examples" / "demo_main.c", "int main(void) { return 0; }\n");
    write_text_file(root / "build" / "generated.c", "int g;\n");

    std::vector<std::string> single_targets = sheen::discover_c_run_targets(ws);
    check(single_targets == std::vector<std::string>{
              (root / "main.cpp").generic_string(),
              (root / "util.c").generic_string(),
              (root / "examples" / "demo_main.c").generic_string(),
              (root / "src" / "engine.cpp").generic_string(),
              (root / "tests" / "unit_main.cpp").generic_string(),
          },
          "discover_c_run_targets lists runnable C/C++ files without fake Makefile target");

    std::vector<std::string> project_sources = sheen::collect_project_c_cxx_sources(ws);
    check(project_sources == std::vector<std::string>{
              (root / "main.cpp").generic_string(),
              (root / "src" / "engine.cpp").generic_string(),
              (root / "util.c").generic_string(),
          },
          "collect_project_c_cxx_sources excludes tests/, examples/, Makefile, and build/");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

int main() {
    test_compiler_diagnostics_gcc_clang_tcc_and_ld();
    test_source_classification_and_build_paths();
    test_compiler_build_plans_pure_and_mixed();
    test_c_run_target_and_project_source_discovery();

    if (g_failures != 0) {
        std::printf("compiler_diagnostics: %llu of %llu checks failed\n",
                    static_cast<unsigned long long>(g_failures),
                    static_cast<unsigned long long>(g_checks));
        return 1;
    }

    std::printf("compiler_diagnostics: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
