/* workspace_store, run on the build machine: verifies atomic persistence, path normalization, recent-path ordering
 * and capping, per-workspace layout and extension isolation, and missing or malformed state files. */

#include "app/workspace_store.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

static constexpr uint32_t MAX_REPORTS = 8;

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* description) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        std::printf("  FAIL: %s\n", description);
    }
}

static std::filesystem::path make_temp_store_dir() {
    std::string base = (std::filesystem::temp_directory_path() / "sheen-store-test-XXXXXX").string();
    std::vector<char> buf(base.begin(), base.end());
    buf.push_back('\0');
    char* created = ::mkdtemp(buf.data());
    return created ? std::filesystem::path(created) : std::filesystem::temp_directory_path() / "sheen-store-fallback";
}

static void test_recent_paths(const std::filesystem::path& dir) {
    sheen::workspace_store store(dir.string());
    check(store.load_recent_paths().empty(), "missing recent.conf loads empty vector");

    check(store.record_recent_path("/sketches/Blink/Blink.ino"), "records first recent path");
    check(store.record_recent_path("/sketches/stellux-leds"), "records workspace folder path");
    check(store.record_recent_path("/sketches/Blink/Blink.ino"), "re-recording moves path to front without duplicate");

    std::vector<std::string> loaded = store.load_recent_paths();
    check(loaded.size() == 2, "deduplicated recent list has 2 entries");
    if (loaded.size() == 2) {
        check(loaded[0] == "/sketches/Blink/Blink.ino", "most recent entry sits first");
        check(loaded[1] == "/sketches/stellux-leds", "older entry sits second");
    }

    for (int i = 0; i < 25; i++) {
        store.record_recent_path("/tmp/file-" + std::to_string(i) + ".c");
    }

    loaded = store.load_recent_paths();
    check(loaded.size() == 20, "recent paths cap at 20 entries");
    if (!loaded.empty()) {
        check(loaded.front() == "/tmp/file-24.c", "newest entry stays at index 0 after cap");
    }

    check(store.clear_recent_paths(), "clear_recent_paths succeeds");
    check(store.load_recent_paths().empty(), "cleared recent list reads back empty");
}

static void test_workspace_roundtrip_and_isolation(const std::filesystem::path& dir) {
    sheen::workspace_store store(dir.string());

    sheen::workspace_record defaults = store.load_workspace("/sketches/stellux-leds/");
    check(defaults.root_path == "/sketches/stellux-leds", "trailing slash is normalized on load");
    check(defaults.enabled_extensions.empty(), "default workspace starts with zero enabled extensions");
    check(defaults.dock_height == 204, "default dock height is 204 px");
    check(!defaults.is_dock_collapsed, "default dock starts uncollapsed");
    check(defaults.is_sidebar_visible, "default sidebar starts visible");

    sheen::workspace_record first;
    first.root_path = "/sketches/stellux-leds/";
    first.enabled_extensions = { "embedded", "python", "embedded" };
    first.dismissed_offers = { "c_run" };
    first.pinned_strip_extension = "embedded";

    first.dock_height = 280;
    first.is_dock_collapsed = true;
    first.active_dock_panel = "monitor";
    first.is_sidebar_visible = false;
    first.sidebar_width = 260;
    check(store.save_workspace(first), "save_workspace writes first workspace");

    sheen::workspace_record second;
    second.root_path = "/work/python-app";
    second.enabled_extensions = { "python" };
    second.dock_height = 180;
    check(store.save_workspace(second), "save_workspace writes second workspace");

    sheen::workspace_record loaded_first = store.load_workspace("/sketches/stellux-leds");
    check(loaded_first.enabled_extensions == std::vector<std::string>{ "embedded", "python" },
          "enabled extensions round-trip deduplicated in order");
    check(loaded_first.dismissed_offers == std::vector<std::string>{ "c_run" }, "dismissed offers round-trip");
    check(loaded_first.pinned_strip_extension == "embedded", "pinned strip extension round-trips");

    check(loaded_first.dock_height == 280, "dock height round-trips");
    check(loaded_first.is_dock_collapsed, "dock collapsed state round-trips");
    check(loaded_first.active_dock_panel == "monitor", "active dock panel round-trips");
    check(!loaded_first.is_sidebar_visible, "sidebar visibility round-trips");
    check(loaded_first.sidebar_width == 260, "sidebar width round-trips");

    sheen::workspace_record loaded_second = store.load_workspace("/work/python-app");
    check(loaded_second.enabled_extensions == std::vector<std::string>{ "python" },
          "second workspace stays isolated from first");
    check(loaded_second.dock_height == 180, "second workspace keeps its own dock height");
}

static void test_corrupt_values_clamp_safely(const std::filesystem::path& dir) {
    sheen::workspace_store store(dir.string());
    std::string key = sheen::workspace_store::workspace_file_key("/tmp/corrupt-ws");
    std::filesystem::path conf_dir = dir / "workspaces";
    std::filesystem::create_directories(conf_dir);

    std::ofstream raw(conf_dir / (key + ".conf"));
    raw << "malformed line without equals\n";
    raw << "dock_height = -999\n";
    raw << "sidebar_width = not_a_number\n";
    raw.close();

    sheen::workspace_record loaded = store.load_workspace("/tmp/corrupt-ws");
    check(loaded.dock_height >= 80, "out-of-range dock_height clamps to minimum");
    check(loaded.sidebar_width == 216, "non-numeric sidebar_width falls back to 216");
}

int main() {
    std::filesystem::path temp_dir = make_temp_store_dir();
    test_recent_paths(temp_dir);
    test_workspace_roundtrip_and_isolation(temp_dir);
    test_corrupt_values_clamp_safely(temp_dir);

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);

    if (g_failures != 0) {
        std::printf("workspace_store: %llu of %llu checks failed\n",
                    static_cast<unsigned long long>(g_failures),
                    static_cast<unsigned long long>(g_checks));
        return 1;
    }

    std::printf("workspace_store: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
