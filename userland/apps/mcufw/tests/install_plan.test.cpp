/* the installer's decisions, run on the build machine: targets, archives and their bounds, entry paths and modes,
 * releases, data tools, and which library releases install where */

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

    check(parse_install_target("Adafruit GFX Library@1.12.1", target, error) && target.kind == install_kind::library,
          "a name without a colon is a library");
    check(target.library == "Adafruit GFX Library" && target.version == "1.12.1", "a library takes a version");
    check(!parse_install_target("esp32:", target, error) &&
              error == "esp32: names no library, core or board: a core is packager:architecture with both parts",
          "an empty part names nothing");
    check(!parse_install_target("esp32:esp32@", target, error), "an empty version fails");
    check(!parse_install_target("a:b:c:d:e", target, error), "too many parts fail");
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

static library_release library_version(const std::string& name, const std::string& version,
                                       const std::vector<library_dependency>& dependencies) {
    return { name, version, { "*" }, dependencies, {} };
}

/* Releases as the library index lists them, with the two cases the design review probed: B needs an older A, and A's
 * newest needs a C that does not exist */
static std::vector<library_release> library_index() {
    return {
        library_version("Adafruit SSD1306", "2.5.15", { { "Adafruit GFX Library", "" } }),
        library_version("Adafruit GFX Library", "1.12.1", { { "Adafruit BusIO", ">=1.14.0" } }),
        library_version("Adafruit GFX Library", "1.11.9", { { "Adafruit BusIO", "" } }),
        library_version("Adafruit BusIO", "1.17.4", {}),
        library_version("Adafruit BusIO", "1.12.0", {}),
        library_version("Adafruit BusIO", "2.0.0", {}),
        library_version("Sensor", "1.0.0", { { "Adafruit BusIO", ">=1.12.0 && <2.0.0" } }),
        library_version("Orphan", "1.0.0", { { "Missing Library", "" } }),
        library_version("Unreadable", "1.0.0", { { "Adafruit BusIO", "~1.0" } }),
        library_version("Equal", "1.0.0", {}),
        library_version("A", "2.0.0", { { "C", "" } }),
        library_version("A", "1.0.0", {}),
        library_version("B", "1.0.0", { { "A", "<2.0.0" } }),
        library_version("C", "1.0.0", {}),
        library_version("D", "2.0.0", { { "E", ">=2.0.0" } }),
        library_version("D", "1.0.0", { { "E", "<2.0.0" } }),
        library_version("F", "1.0.0", { { "D", "" } }),
        library_version("E", "1.0.0", {}),
    };
}

static install_target library_target(const std::string& name, const std::string& version) {
    install_target target;
    target.kind = install_kind::library;
    target.library = name;
    target.version = version;

    return target;
}

static library installed_library(const std::string& name, const std::string& version, bool is_legacy = false) {
    return { name, version, "/libraries/" + library_folder_name(name), library_folder_name(name),
             library_location::installed, is_legacy, library_layout::flat, {}, {}, "", false, "" };
}

static std::string chosen_text(const std::vector<release_name>& chosen) {
    std::string text;
    for (const release_name& release : chosen) {
        text += (text.empty() ? "" : ", ") + release.name + " " + release.version;
    }

    return text;
}

static bool resolves(const std::vector<install_target>& targets, const std::vector<library>& installed,
                     const std::string& expected) {
    std::vector<release_name> chosen;
    std::string error;
    if (resolve_libraries(library_index(), targets, installed, chosen, error) && chosen_text(chosen) == expected) {
        return true;
    }

    printf("  got: %s\n", error.empty() ? chosen_text(chosen).c_str() : error.c_str());

    return false;
}

static bool refuses(const std::vector<install_target>& targets, const std::vector<library>& installed,
                    const std::string& expected) {
    std::vector<release_name> chosen;
    std::string error;
    if (!resolve_libraries(library_index(), targets, installed, chosen, error) && error == expected) {
        return true;
    }

    printf("  got: %s\n", error.empty() ? chosen_text(chosen).c_str() : error.c_str());

    return false;
}

static void check_libraries() {
    check(resolves({ library_target("Adafruit SSD1306", "") }, {},
                   "Adafruit BusIO 2.0.0, Adafruit GFX Library 1.12.1, Adafruit SSD1306 2.5.15"),
          "a chain resolves to the newest of each, dependencies first");
    check(resolves({ library_target("Adafruit SSD1306", ""), library_target("Sensor", "") }, {},
                   "Adafruit BusIO 1.17.4, Adafruit GFX Library 1.12.1, Adafruit SSD1306 2.5.15, Sensor 1.0.0"),
          "two targets resolve together to the newest release meeting both");
    check(resolves({ library_target("Adafruit GFX Library", "1.11.9") }, {},
                   "Adafruit BusIO 2.0.0, Adafruit GFX Library 1.11.9"),
          "a target's version holds");

    check(resolves({ library_target("A", ""), library_target("B", "") }, {}, "A 1.0.0, B 1.0.0"),
          "a target another one needs older takes that release, leaving out what its newest needed");
    check(resolves({ library_target("F", "") }, {}, "E 1.0.0, D 1.0.0, F 1.0.0"),
          "a dependency whose newest cannot be met backtracks to one that can");
}

static void check_installed_libraries() {
    std::vector<library> old_bus = { installed_library("Adafruit BusIO", "1.12.0") };
    check(resolves({ library_target("Sensor", "") }, old_bus, "Sensor 1.0.0"),
          "an installed library meeting the constraints stays");
    check(refuses({ library_target("Adafruit GFX Library", "") }, old_bus,
                  "Adafruit BusIO 1.12.0 is installed, but Adafruit GFX Library 1.12.1 needs >=1.14.0, so run mcufw "
                  "install \"Adafruit BusIO@2.0.0\""),
          "an installed library failing a constraint names the dependent and the install that fixes it");
    check(resolves({ library_target("Adafruit BusIO", "") }, old_bus, "Adafruit BusIO 2.0.0"),
          "naming an installed library installs its newest");

    check(resolves({ library_target("Adafruit BusIO", "") }, { installed_library("Adafruit BusIO", "2.0.0") }, ""),
          "naming a library installed at its newest installs nothing");
    check(resolves({ library_target("Equal", "") }, { installed_library("Equal", "1.0") }, ""),
          "an installed 1.0 is the index's 1.0.0, so nothing installs");
    check(resolves({ library_target("Sensor", "") }, { installed_library("Adafruit BusIO", "1.15.0") }, "Sensor 1.0.0"),
          "an installed version the index lacks stays when it meets the constraints");

    check(refuses({ library_target("Sensor", "") }, { installed_library("Adafruit BusIO", "", true) },
                  "Adafruit BusIO is installed, but the libraries being installed need 1.17.4, so run mcufw install "
                  "\"Adafruit BusIO@1.17.4\""),
          "a legacy library of a needed name is never replaced unasked");
}

static void check_library_refusals() {
    check(refuses({ library_target("Adafruit BusIO", "3.0.0") }, {}, "the library index lists no Adafruit BusIO 3.0.0"),
          "a version the index lacks fails");
    check(refuses({ library_target("esp32da", "") }, {},
                  "the library index lists no library esp32da, and a core or board is named with colons, as "
                  "esp32:esp32:esp32da"),
          "a word the index does not list says how a core or board is named");
    check(refuses({ library_target("Orphan", "") }, {},
                  "the library index lists no library Missing Library, which Orphan 1.0.0 depends on"),
          "a dependency the index lacks fails, naming the dependent");
    check(refuses({ library_target("Unreadable", "") }, {},
                  "Unreadable 1.0.0's dependency on Adafruit BusIO: cannot read the version constraint \"~1.0\""),
          "a constraint outside relaxed-semver's grammar fails, naming the dependency");
    check(refuses({ library_target("Adafruit BusIO", "1.12.0"), library_target("Adafruit BusIO", "2.0.0") }, {},
                  "two different versions of the library Adafruit BusIO are required: 1.12.0 and 2.0.0"),
          "a library named at two versions fails");
    check(refuses({ library_target("Adafruit GFX Library", "1.12.1"), library_target("Adafruit BusIO", "1.12.0") }, {},
                  "no library releases meet every dependency of Adafruit GFX Library, Adafruit BusIO"),
          "pinned targets that cannot go together fail");
}

static void check_library_index_need() {
    install_target core;
    core.kind = install_kind::core;
    install_target board;
    board.kind = install_kind::board;
    check(!needs_library_index({ core, board }) && !needs_library_index({}),
          "an install of cores and boards alone goes on without the library index");
    check(needs_library_index({ core, library_target("FastLED", "") }),
          "an install naming a library fails without the library index");
}

static void check_library_installs() {
    std::vector<library_install> installs;
    std::string error;

    const std::string libraries = "/libraries";
    const std::vector<std::string> no_entries;
    const std::vector<library> none_installed;
    const std::vector<release_name> gfx = { { "Adafruit GFX Library", "1.12.1" } };
    check(plan_library_installs(gfx, none_installed, libraries, no_entries, installs, error) && installs.size() == 1 &&
              installs[0].directory == "/libraries/Adafruit_GFX_Library" && installs[0].replaced.empty(),
          "a new library lands in its folder name");

    const std::vector<release_name> bus = { { "Adafruit BusIO", "2.0.0" } };
    const std::vector<std::string> bus_entry = { "Adafruit_BusIO" };
    const std::vector<library> in_place = { installed_library("Adafruit BusIO", "1.12.0") };
    check(plan_library_installs(bus, in_place, libraries, bus_entry, installs, error) &&
              installs[0].replaced == "/libraries/Adafruit_BusIO",
          "an installed copy in the same folder is replaced");

    library elsewhere = installed_library("Adafruit BusIO", "1.12.0");
    elsewhere.directory = "/libraries/BusIO-master";
    const std::vector<library> moved = { elsewhere };
    const std::vector<std::string> moved_entry = { "BusIO-master" };
    check(plan_library_installs(bus, moved, libraries, moved_entry, installs, error) &&
              installs[0].directory == "/libraries/Adafruit_BusIO" &&
              installs[0].replaced == "/libraries/BusIO-master",
          "an installed copy in another folder is replaced, the new release landing in its own folder name");

    library other = installed_library("Other Library", "1.0.0");
    other.directory = "/libraries/Adafruit_BusIO";
    const std::vector<library> placed = { other };
    check(!plan_library_installs(bus, placed, libraries, bus_entry, installs, error) &&
              error == "/libraries/Adafruit_BusIO already holds something other than Adafruit BusIO, so mcufw cannot "
                       "install it there",
          "a folder a person placed holding another library is left alone");
    check(!plan_library_installs(bus, none_installed, libraries, bus_entry, installs, error),
          "a folder holding no library is left alone too");

    const std::vector<release_name> colliding = { { "Foo Bar", "1.0.0" }, { "Foo_Bar", "1.0.0" } };
    check(!plan_library_installs(colliding, none_installed, libraries, no_entries, installs, error) &&
              error == "/libraries/Foo_Bar already holds something other than Foo_Bar, so mcufw cannot install it "
                       "there",
          "two chosen names that share a folder name fail");

    library twin = installed_library("Adafruit BusIO", "1.17.4");
    twin.directory = "/libraries/BusIO-copy";
    const std::vector<library> twins = { in_place[0], twin };
    check(!plan_library_installs(bus, twins, libraries, no_entries, installs, error) &&
              error == "Adafruit BusIO is installed twice, in /libraries/Adafruit_BusIO and /libraries/BusIO-copy, "
                       "so remove one before installing it",
          "a library installed twice fails, as arduino-cli refuses it");
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

    check_libraries();
    check_installed_libraries();
    check_library_refusals();
    check_library_index_need();
    check_library_installs();

    if (g_failures != 0) {
        printf("install_plan: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("install_plan: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
