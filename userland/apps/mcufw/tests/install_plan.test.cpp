/* the installer's decisions, run on the build machine: what a target names, which archives install and how much they
 * may hold, where entries land, which modes apply, which release installs, which tools download, and which stay */

#include "installer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;
constexpr const char* CHECKSUM = "SHA-256:87401004f279a206781e368b66098a8383b7cf87cce7811a6afd0163503fa78f";
constexpr const char* DIGEST = "87401004f279a206781e368b66098a8383b7cf87cce7811a6afd0163503fa78f";
constexpr const char* UPPERCASE_CHECKSUM = "SHA-256:87401004F279A206781E368B66098A8383B7CF87CCE7811A6AFD0163503FA78F";
constexpr size_t PATH_MAX_BYTES = 4096;
constexpr size_t NAME_MAX_BYTES = 255;

static uint64_t g_checks;
static uint64_t g_failures;

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

static void check_targets() {
    install_target target;
    std::string error;
    check(parse_install_target("esp32:esp32", target, error) && target.kind == install_kind::core, "a core");
    check(target.board.packager == "esp32" && target.board.architecture == "esp32" && target.version.empty(),
          "a core names its packager and architecture");

    check(parse_install_target("esp32:esp32:esp32p4:ChipVariant=esp32p4_es@3.3.12", target, error), "a board");
    check(target.kind == install_kind::board && target.board.board_id == "esp32p4" && target.version == "3.3.12",
          "a board takes options and a version");
    check(target.board.options.size() == 1 && target.board.options[0].choice_id == "esp32p4_es", "its option");

    check(!parse_install_target("esp32da", target, error), "a board ID alone fails");
    check(error == "esp32da names no core or board, so name a core as packager:architecture or a board by its FQBN",
          "a board ID alone says how to name one");
    check(!parse_install_target("esp32:esp32@", target, error), "an empty version fails");
}

static void check_archives() {
    std::string sha256;
    std::string error;
    check(check_archive({ "https://x/esp32-core.zip", "esp32-core.zip", CHECKSUM, 1 }, sha256, error) &&
              sha256 == DIGEST,
          "a zip with SHA-256 passes, giving its digest");
    check(check_archive({ "https://x/a.zip", "a.zip", UPPERCASE_CHECKSUM, 1 }, sha256, error) && sha256 == DIGEST,
          "an uppercase digest passes in lowercase, as Go's hex reader gives arduino-cli");

    check(!check_archive({ "https://x/CMSIS.tar.bz2", "CMSIS.tar.bz2", CHECKSUM, 1 }, sha256, error) &&
              error == "CMSIS.tar.bz2 is no zip, which is the only archive mcufw unpacks",
          "a tarball fails");
    check(!check_archive({ "", "a.zip", CHECKSUM, 1 }, sha256, error) && error == "the index gives no URL for a.zip",
          "an archive without a URL fails");
    check(!check_archive({ "https://x/a.zip", "a.zip", CHECKSUM, 0 }, sha256, error) &&
              error == "the index states no size for a.zip, which bounds what mcufw unpacks",
          "an archive without a size fails");

    const char* checksums[] = {
        "MD5:0123456789abcdef0123456789abcdef",
        "SHA-256:87401004",
        "SHA-256:87401004f279a206781e368b66098a8383b7cf87cce7811a6afd0163503fa78g",
    };

    for (const char* checksum : checksums) {
        check(!check_archive({ "https://x/a.zip", "a.zip", checksum, 1 }, sha256, error) &&
                  error == "a.zip has no SHA-256 checksum in the index, so mcufw cannot verify it",
              checksum);
    }
}

static void check_unpacked_bytes() {
    check(unpacked_bytes_max({ "https://x/a.zip", "a.zip", CHECKSUM, 100 }) == 1600,
          "an archive may unpack to 16 times its stated size");
    check(unpacked_bytes_max({ "https://x/a.zip", "a.zip", CHECKSUM, UINT64_MAX / 16 + 1 }) == UINT64_MAX,
          "a cap past 64 bits stops at the largest size");
}

static void check_received_sizes() {
    archive_download archive = { "https://x/a.zip", "a.zip", CHECKSUM, 100 };
    std::string error;
    check(check_received_size(archive, 100, false, error), "bytes up to the stated size pass");
    check(!check_received_size(archive, 101, false, error), "a byte past the stated size fails as it arrives");
    check(error == "https://x/a.zip holds more than the 100 bytes its index states", "the excess names the URL");

    check(check_received_size(archive, 100, true, error), "a download of exactly the stated size passes");
    check(!check_received_size(archive, 99, true, error), "a download short of the stated size fails");
    check(error == "https://x/a.zip holds 99 of the 100 bytes its index states", "the shortfall is counted");
}

static void check_entry_paths() {
    std::string top;
    std::string staged;
    std::string error;
    check(staged_entry_path("esp32-3.3.12/", top, staged, error) && top == "esp32-3.3.12" && staged.empty(),
          "the top folder is kept and dropped");
    check(staged_entry_path("esp32-3.3.12/cores/esp32/main.cpp", top, staged, error) &&
          staged == "cores/esp32/main.cpp", "an entry below it lands below the staged folder");

    const char* unsafe[] = { "../x", "/x", "esp32-3.3.12/a/../../x", "esp32-3.3.12/./x", "esp32-3.3.12//x",
                             "esp32-3.3.12/a\\b" };
    for (const char* path : unsafe) {
        check(!staged_entry_path(path, top, staged, error) && error.starts_with("the archive names an unsafe path"),
              path);
    }

    check(!staged_entry_path(std::string_view("esp32-3.3.12/a\0b", 16), top, staged, error), "a NUL fails");
    check(!staged_entry_path("other/x", top, staged, error), "a second top folder fails");
    check(error == "other/x lies outside the archive's top folder, esp32-3.3.12", "and names the first");

    std::string fresh;
    check(!staged_entry_path("README.md", fresh, staged, error), "a file beside the top folder fails");
    check(error == "README.md lies outside the archive's top folder", "a file beside the top folder is named");
}

/* A path below the top folder `t` that is exactly `size` bytes long, its parts at most 255 bytes */
static std::string path_of_size(size_t size) {
    std::string path = "t";
    while (path.size() < size) {
        size_t part = std::min<size_t>(NAME_MAX_BYTES, size - path.size() - 1);
        path += "/" + std::string(part, 'a');
    }

    return path;
}

static void check_entry_limits() {
    std::string top;
    std::string staged;
    std::string error;
    check(staged_entry_path(path_of_size(PATH_MAX_BYTES), top, staged, error), "a path of 4096 bytes passes");
    check(!staged_entry_path(path_of_size(PATH_MAX_BYTES + 1), top, staged, error) &&
              error == "the archive names a path longer than 4096 bytes",
          "a path of 4097 bytes fails");

    std::string longest_name = "t/" + std::string(NAME_MAX_BYTES, 'a');
    check(staged_entry_path(longest_name, top, staged, error), "a part of 255 bytes passes");
    check(!staged_entry_path(longest_name + "a", top, staged, error) &&
              error == "the archive names a path with a part longer than 255 bytes",
          "a part of 256 bytes fails");
}

static void check_member_modes() {
    std::vector<zip_member> members = {
        { "esp32/", 040755 },
        { "esp32/tools/esptool", 0100755 },
        { "esp32/tools/setuid", 04755 },
        { "esp32/tools/setgid", 02755 },
        { "esp32/platform.txt", 0100644 },
        { "esp32/made-on-dos.exe", 0 },
    };

    std::vector<std::string> executables;
    std::string error;
    check(check_member_modes(members, executables, error), "an archive without links passes");
    check(executables == std::vector<std::string>({ "tools/esptool", "tools/setuid", "tools/setgid" }),
          "files with an execute bit are marked by staged path alone, so no other mode bit applies");

    for (uint32_t link_mode : { 0120777u, 0120000u }) {
        std::vector<zip_member> with_link = { { "esp32/", 040755 }, { "esp32/link", link_mode } };
        check(!check_member_modes(with_link, executables, error) &&
                  error == "the archive holds a symbolic link, esp32/link, which mcufw does not install",
              "a symbolic link fails, with or without permission bits");
    }
}

static core_release release(const std::string& version) {
    return { "esp32", "esp32", version, "esp32", { "https://x/a.zip", "a.zip", CHECKSUM, 1 }, {} };
}

static void check_releases() {
    std::vector<core_release> releases = { release("3.3.2"), release("3.3.12"), release("3.3.11") };
    const core_release* chosen = nullptr;
    std::string error;
    check(choose_core_release(releases, "", {}, chosen, error) && chosen->version == "3.3.12", "the newest");
    check(choose_core_release(releases, "3.3.2", {}, chosen, error) && chosen->version == "3.3.2", "the one given");
    check(choose_core_release(releases, "", { "3.3.11", "3.3.2" }, chosen, error) && chosen->version == "3.3.11",
          "the newest with overrides");

    check(!choose_core_release(releases, "3.3.12", { "3.3.11" }, chosen, error), "one without overrides fails");
    check(error == "mcufw installs esp32:esp32 only at 3.3.11, the version Stellux has overrides for, and the "
                   "index lists no 3.3.12 among them", "naming the version that installs");
    check(!choose_core_release(releases, "9.9.9", {}, chosen, error), "one the index lacks fails");
    check(error == "the package index lists no esp32:esp32 9.9.9", "and says so");
}

static void check_tool_plans() {
    std::vector<tool_release> tools = {
        { "esp32", "esp32-libs", "3.3.12", true, { "https://x/libs.zip", "libs.zip", CHECKSUM, 1 } },
        { "esp32", "esp-x32", "2601", false, {} },
    };

    board_requirements requirements;
    requirements.missing_tools = { { "esp32", "esp32-libs", "3.3.12" }, { "esp32", "esp-x32", "2601" } };
    std::vector<const tool_release*> data_tools;
    std::string error;
    check(plan_data_tools(requirements, tools, data_tools, error), "the plan holds");
    check(data_tools.size() == 1 && data_tools[0]->name == "esp32-libs", "only data tools download");

    board_requirements undeclared = requirements;
    undeclared.missing_tools.push_back({ "", "gamma-sdk", "" });
    check(!plan_data_tools(undeclared, tools, data_tools, error), "a tool the core does not depend on fails");
    check(error == "the board's recipes name tool gamma-sdk, which the core does not depend on", "naming it");

    requirements.missing_tools.push_back({ "esp32", "esp-gone", "1.0" });
    check(!plan_data_tools(requirements, tools, data_tools, error), "a dependency the index lacks fails");
    check(error == "the board's recipes name tool esp32:esp-gone 1.0, which the package index does not list",
          "naming the dependency");
}

static void check_unused_tools() {
    installed_core core;
    core.tools = { { "esp32", "esp32-libs", "3.3.12" }, { "esp32", "esp-x32", "2601" } };
    std::vector<tool_dependency> installed = { { "esp32", "esp32-libs", "3.3.12" }, { "esp32", "esp32-libs", "3.3.11" },
                                               { "esp32", "esp32s3-libs", "3.3.12" } };

    std::vector<tool_dependency> unused = unused_data_tools({ core }, installed);
    check(unused.size() == 2 && unused[0].version == "3.3.11" && unused[1].name == "esp32s3-libs",
          "tools no record names go, even another version of a named one");
    check(unused_data_tools({}, installed).size() == 3, "with no core every tool goes");
}

int main() {
    check_targets();
    check_archives();
    check_received_sizes();
    check_unpacked_bytes();
    check_entry_paths();
    check_entry_limits();

    check_member_modes();
    check_releases();
    check_tool_plans();
    check_unused_tools();

    if (g_failures != 0) {
        printf("install_plan: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("install_plan: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
