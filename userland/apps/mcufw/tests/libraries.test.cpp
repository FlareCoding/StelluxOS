/* libraries, run on the build machine: library folder names as arduino-cli writes them, and the libraries found in a
 * core's folder and in mcufw's, named by their properties or their folders */

#include "libraries.hpp"

#include "files.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

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

static void write_text(const std::string& path, const std::string& text) {
    FILE* file = fopen(path.c_str(), "w");
    fputs(text.c_str(), file);
    fclose(file);
}

static void check_folder_names() {
    check(library_folder_name("Adafruit SSD1306") == "Adafruit_SSD1306", "a space becomes an underscore");
    check(library_folder_name("Adafruit GFX Library") == "Adafruit_GFX_Library", "every space does");
    check(library_folder_name("ESP32-BLE.Keyboard") == "ESP32-BLE.Keyboard", "dashes and dots stay");
    check(library_folder_name("-x.y") == "_x.y" && library_folder_name(".hidden") == "_hidden",
          "a dash or dot first becomes an underscore");

    check(library_folder_name("Caf\xc3\xa9+") == "Caf__", "each other character becomes one underscore");
    check(library_folder_name("\xe6\x97\xa5\xe6\x9c\xac") == "__", "a character of three bytes is one underscore");
    check(library_folder_name("a\xff" "b") == "a_b", "a byte that starts no character is one underscore");
    check(library_folder_name(std::string(70, 'a')) == std::string(64, 'a'), "a folder name stops at 64 bytes");
}

static void check_found(const std::string& root) {
    std::string core = join_path(root, "core");
    std::string placed = join_path(root, "libraries");
    for (const char* folder : { "core/Network", "core/Wire", "core/empty", "libraries/Adafruit_BusIO",
                                "libraries/unnamed", "libraries/.hidden" }) {
        mkdir(join_path(root, folder).c_str(), 0755);
    }

    write_text(join_path(core, "Network/library.properties"), "name=Networking\nversion=3.3.12\n");
    write_text(join_path(core, "Wire/Wire.h"), "");
    write_text(join_path(core, "empty/notes.txt"), "");
    write_text(join_path(placed, "Adafruit_BusIO/library.properties"), "name=Adafruit BusIO\nversion=1.17.4\n");
    write_text(join_path(placed, "unnamed/library.properties"), "version=1.0.0\n");
    write_text(join_path(placed, ".hidden/library.properties"), "name=Hidden\n");

    directory_layout layout;
    layout.libraries = placed;
    std::vector<library> libraries;
    std::string error;
    check(find_libraries(core, layout, libraries, error) && libraries.size() == 4,
          "both folders are read, without a folder holding neither properties nor a header, nor a hidden one");
    if (libraries.size() != 4) {
        return;
    }

    check(libraries[0].name == "Networking" && libraries[0].folder_name == "Network", "a name from its properties");
    check(libraries[0].version == "3.3.12" && libraries[0].location == library_location::core, "a core's library");
    check(libraries[1].name == "Wire" && libraries[1].version.empty() && libraries[1].is_legacy,
          "a folder holding a header without properties is a legacy library named by its folder");
    check(libraries[2].name == "Adafruit BusIO" && libraries[2].location == library_location::installed,
          "the installed libraries follow the core's");
    check(libraries[3].name == "-" && libraries[3].version == "1.0.0" && !libraries[3].is_legacy,
          "properties without a name give the name -, as arduino-cli loads them");

    check(find_libraries("", layout, libraries, error) && libraries.size() == 2, "no core folder means none from it");
    layout.libraries = join_path(root, "missing");
    check(find_libraries("", layout, libraries, error) && libraries.empty(), "a missing folder holds none");
}

int main() {
    char root_template[] = "/tmp/mcufw-libraries-XXXXXX";
    std::string root = mkdtemp(root_template);
    mkdir(join_path(root, "core").c_str(), 0755);
    mkdir(join_path(root, "libraries").c_str(), 0755);

    check_folder_names();
    check_found(root);

    std::string cleanup = "rm -rf " + root;
    if (system(cleanup.c_str()) != 0) {
        printf("libraries: could not delete %s\n", root.c_str());
    }

    if (g_failures != 0) {
        printf("libraries: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("libraries: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
