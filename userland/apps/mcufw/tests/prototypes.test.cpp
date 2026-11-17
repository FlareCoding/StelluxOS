/* prototypes, run on the build machine: each case in fixtures/prototypes gives the .ino.cpp arduino-cli 1.6.0 wrote
 * for its merged sketch, and the plan keeps `static`, `extern "C"` and default arguments as arduino-cli's rules do */

#include "files.hpp"
#include "prototypes.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

constexpr const char* CASES = "fixtures/prototypes";

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const std::string& what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what.c_str());
    }
}

// The .ino files a merge names in its `#line 1` marks, the main file first, each with the text the merge holds for
// it, which is the file's own text, as arduino-cli's checks read it from disk
static std::vector<ino_text> merged_ino_files(const std::string& merged) {
    constexpr std::string_view mark = "#line 1 \"";
    std::vector<ino_text> files;
    size_t start = 0;
    while (start < merged.size()) {
        size_t end = merged.find('\n', start);
        std::string_view line = std::string_view(merged).substr(start, end - start);
        start = end == std::string::npos ? merged.size() : end + 1;
        if (line.substr(0, mark.size()) == mark) {
            files.push_back({ std::string(line.substr(mark.size(), line.size() - mark.size() - 1)), "" });
        } else if (!files.empty()) {
            files.back().text += std::string(line) + "\n";
        }
    }

    return files;
}

// The lines before the merge's first `#line 1` mark, and the mark itself, as merge_ino_files counts them
static uint32_t merged_line_offset(const std::string& merged) {
    size_t mark = merged.find("#line 1 \"");
    uint32_t lines_before = static_cast<uint32_t>(std::count(merged.begin(), merged.begin() + mark, '\n'));

    return lines_before + 1;
}

static void check_case(const std::string& name) {
    std::string directory = join_path(CASES, name);
    std::string preprocessed;
    std::string merged_text;
    std::string expected;
    std::string error;

    bool loaded = read_file(join_path(directory, "preprocessed.ii"), preprocessed, error) &&
                  read_file(join_path(directory, "merged.txt"), merged_text, error) &&
                  read_file(join_path(directory, "expected.txt"), expected, error);
    check(loaded, name + ": " + error);
    if (!loaded) {
        return;
    }

    std::vector<ino_text> files = merged_ino_files(merged_text);
    std::vector<std::string> ino_files;
    for (const ino_text& file : files) {
        ino_files.push_back(file.path);
    }

    bool line_too_long = false;
    std::string source;
    merged_sketch merged = { merged_text, merged_line_offset(merged_text) };
    std::string lines = sketch_lines(preprocessed, ino_files, line_too_long);
    prototype_plan plan = find_prototypes(lines, ino_files.front(), files);
    check(insert_prototypes(merged, plan, source) && source == expected, name + ": the .ino.cpp is arduino-cli's");
}

static void check_cases() {
    std::vector<std::string> names;
    std::string error;
    check(list_directory(CASES, names, error) && names.size() >= 10, "the cases are listed");
    for (const std::string& name : names) {
        check_case(name);
    }
}

static void check_sketch_lines() {
    std::string preprocessed = "# 1 \"/s/A/A.ino.cpp.merged\"\n"
                               "# 1 \"/core/Arduino.h\" 1\n"
                               "int core_value;\n"
                               "# 2 \"/s/A/A.ino.cpp.merged\" 2\n"
                               "# 1 \"/s/A/A.ino\"\r\n"
                               "void setup() {}\r\n"
                               "# 1 \"/s/A/B.ino\"\n"
                               "void helper() {}\n"
                               "#  2 \"/s/A/A.ino\"\n"
                               "int kept_after_a_malformed_marker;\n";
    std::string expected = "# 1 \"/s/A/A.ino\"\n"
                           "void setup() {}\n"
                           "# 1 \"/s/A/B.ino\"\n"
                           "void helper() {}\n"
                           "#  2 \"/s/A/A.ino\"\n"
                           "int kept_after_a_malformed_marker;\n";
    bool line_too_long = true;
    check(sketch_lines(preprocessed, { "/s/A/A.ino", "/s/A/B.ino" }, line_too_long) == expected && !line_too_long,
          "sketch_lines keeps the .ino files' lines and markers, drops carriage returns, and ignores a marker "
          "arduino-cli does not parse");

    std::string longest = "# 1 \"/s/A/A.ino\"\n" + std::string(1024 * 1024 - 1, 'x') + "\nint after;\n";
    check(sketch_lines(longest, { "/s/A/A.ino" }, line_too_long).ends_with("int after;\n") && !line_too_long,
          "a line just under 1 MiB is read");
    std::string too_long = "# 1 \"/s/A/A.ino\"\n" + std::string(1024 * 1024, 'x') + "\nint after;\n";
    check(sketch_lines(too_long, { "/s/A/A.ino" }, line_too_long) == "# 1 \"/s/A/A.ino\"\n" && line_too_long,
          "a line of 1 MiB stops the reading, as arduino-cli's filter does");

    std::string body = "void before() {}\n" + std::string(1024 * 1024, 'x') + "\nvoid after() {}\n";
    std::string kept = sketch_lines("# 1 \"/s/A/A.ino\"\n" + body, { "/s/A/A.ino" }, line_too_long);
    prototype_plan plan = find_prototypes(kept, "/s/A/A.ino", { { "/s/A/A.ino", body } });
    check(line_too_long && plan.prototypes.size() == 1 && plan.prototypes[0].declaration == "void before();",
          "past a line too long, prototypes come from the lines before it, as arduino-cli goes on with what it read");

    merged_sketch merged = { "#include <Arduino.h>\n#line 1 \"/s/A/A.ino\"\n" + body, 2 };
    std::string inserted;
    check(insert_prototypes(merged, plan, inserted) && inserted.find("void before();") != std::string::npos,
          "and they are still inserted, so the build goes on");
}

static const function_prototype* find_declaration(const prototype_plan& plan, const std::string& declaration) {
    for (const function_prototype& prototype : plan.prototypes) {
        if (prototype.declaration == declaration) {
            return &prototype;
        }
    }

    return nullptr;
}

static void check_plan() {
    std::string lines = "# 1 \"/s/A/A.ino\"\n"
                        "static int counter;\n"
                        "void declared(int);\n"
                        "struct Point { int sum() const { return 0; } };\n"
                        "int registration = attach(&on_event);\n"
                        "static void bump(void) { counter++; }\n"
                        "extern \"C\" {\n"
                        "void c_side(int x) {}\n"
                        "}\n"
                        "void blink(int times = 2) {}\n"
                        "void declared(int value) {}\n"
                        "void on_event() {}\n";
    std::vector<ino_text> files = { { "/s/A/A.ino", lines.substr(lines.find('\n') + 1) } };
    prototype_plan plan = find_prototypes(lines, "/s/A/A.ino", files);

    const function_prototype* bump = find_declaration(plan, "void bump(void);");
    check(bump != nullptr && bump->modifiers == "static" && bump->line == 5, "a static definition keeps `static`");

    const function_prototype* c_side = find_declaration(plan, "void c_side(int x);");
    check(c_side != nullptr && c_side->modifiers == " extern \"C\"", "a definition in `extern \"C\"` keeps it");

    check(find_declaration(plan, "void blink(int times = 2);") != nullptr, "the plan keeps a default argument");
    check(find_declaration(plan, "void declared(int value);") != nullptr, "a definition unlike its declaration stays");
    check(find_declaration(plan, " int sum() const;") == nullptr &&
              find_declaration(plan, "int sum() const;") == nullptr,
          "a member function gets no prototype");
    check(plan.insert_line == 4, "a function's address taken above the first function moves the insertion up");

    merged_sketch merged = { "#include <Arduino.h>\n#line 1 \"/s/A/A.ino\"\n" + lines.substr(lines.find('\n') + 1), 2 };
    std::string inserted;
    check(insert_prototypes(merged, plan, inserted) &&
              inserted.find("blink(int times") == inserted.rfind("blink(int times"),
          "insert_prototypes leaves out the prototype with a default argument");

    plan.insert_line = 40;
    check(!insert_prototypes(merged, plan, inserted),
          "an insertion line past the merge writes nothing, as in arduino-cli");
}

int main() {
    check_cases();
    check_sketch_lines();
    check_plan();

    if (g_failures != 0) {
        printf("prototypes: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("prototypes: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
