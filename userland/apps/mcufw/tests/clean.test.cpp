/* clean_sketch, run on the build machine in a temporary layout: a sketch's build tree deleted under its lock, a tree a
 * build holds left alone, the tree of a sketch whose folder is gone, and the core cache deleted under the state lock */

#include "clean.hpp"

#include "build.hpp"
#include "build_plan.hpp"
#include "file_lock.hpp"
#include "files.hpp"

#include <unistd.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

static uint64_t    g_checks;
static uint64_t    g_failures;
static std::string g_root;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static void write_text(const std::string& path, const std::string& text) {
    std::string error;
    FILE* file = make_directories(parent_directory(path), error) ? fopen(path.c_str(), "wb") : nullptr;
    if (!file || fwrite(text.data(), 1, text.size(), file) != text.size()) {
        printf("  cannot write %s\n", path.c_str());
        exit(1);
    }

    fclose(file);
}

static directory_layout test_layout() {
    directory_layout layout;
    layout.builds = g_root + "/builds";
    layout.state = g_root + "/state";
    layout.core_cache = g_root + "/cache/cores";
    return layout;
}

static void check_clean() {
    directory_layout layout = test_layout();
    std::string sketch = g_root + "/sketches/Lamp";
    std::string tree = build_directory_for(layout, sketch);
    write_text(sketch + "/Lamp.ino", "void setup() {}\n");
    write_text(tree + "/sketch/Lamp.ino.cpp", "void setup() {}\n");

    file_lock held;
    std::string error;
    failure failed;
    check(held.acquire(join_path(tree, BUILD_LOCK_NAME), lock_mode::exclusive, error), "a build holds the tree's lock");
    check(!clean_sketch(layout, sketch, false, failed) && failed.kind == failure_kind::lock && is_directory(tree),
          "leaves a tree a build holds");

    held.release();
    std::vector<std::string> names;
    check(clean_sketch(layout, sketch + "/Lamp.ino", false, failed) && !is_directory(tree),
          "deletes the tree a main file names");
    check(list_directory(layout.builds, names, error) && names.empty(), "leaves nothing renamed aside");
    check(clean_sketch(layout, sketch, false, failed), "finds nothing to delete the second time");

    write_text(tree + "/sketch/Lamp.ino.cpp", "void setup() {}\n");
    std::string link = g_root + "/linked";
    check(symlink((g_root + "/sketches").c_str(), link.c_str()) == 0 &&
              clean_sketch(layout, link + "/Lamp", false, failed) && !is_directory(tree),
          "deletes the tree of a sketch named through a symlink, resolved as a build resolves it");

    std::string gone = g_root + "/sketches/Gone";
    std::string gone_tree = build_directory_for(layout, gone);
    write_text(gone_tree + "/core/core.a", "archive");
    check(clean_sketch(layout, gone + "/Gone.ino", false, failed) && !is_directory(gone_tree),
          "deletes the tree of a sketch whose folder is gone");
}

static void check_clean_cores() {
    directory_layout layout = test_layout();
    std::string sketch = g_root + "/sketches/Lamp";
    std::string tree = build_directory_for(layout, sketch);
    write_text(tree + "/sketch/Lamp.ino.cpp", "void setup() {}\n");
    write_text(layout.core_cache + "/0123/core.a", "archive");

    file_lock linking;
    std::string error;
    failure failed;
    check(make_directories(layout.state, error) &&
              linking.acquire(join_path(layout.state, STATE_LOCK_NAME), lock_mode::shared, error),
          "a build links from the cache");
    check(!clean_sketch(layout, sketch, true, failed) && failed.kind == failure_kind::lock && is_directory(tree) &&
              is_directory(layout.core_cache),
          "leaves the tree and the cache while a build links from the cache");

    linking.release();
    check(clean_sketch(layout, sketch, true, failed) && !is_directory(tree) && !is_directory(layout.core_cache),
          "deletes the tree and the cache");
}

int main() {
    char made[] = "/tmp/mcufw-clean-XXXXXX";
    char resolved[PATH_MAX];
    if (!mkdtemp(made) || !realpath(made, resolved)) {
        printf("  cannot make a temporary folder\n");
        return 1;
    }

    g_root = resolved;
    check_clean();
    check_clean_cores();

    std::string error;
    if (!remove_tree(g_root, error)) {
        printf("  %s\n", error.c_str());
    }

    if (g_failures != 0) {
        printf("clean: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("clean: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
