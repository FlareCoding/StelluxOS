/* libraries, run on the build machine: library folder names as arduino-cli writes them, the libraries found in a
 * core's folder and in mcufw's with what arduino-cli's loader reads of each, and arduino-cli's ranking */

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

static void write_text(const std::string& path, const std::string& text) {
    FILE* file = fopen(path.c_str(), "w");
    fputs(text.c_str(), file);
    fclose(file);
}

static void write_tree(const std::string& root, const std::vector<std::string>& files) {
    for (const std::string& file : files) {
        std::string path = join_path(root, file);
        std::string error;
        make_directories(parent_directory(path), error);
        write_text(path, file.ends_with("library.properties") ? "" : "\n");
    }
}

static library ranked(const std::string& name, const std::string& folder, std::vector<std::string> architectures,
                      library_location location) {
    library made = {};
    made.name = name;
    made.folder_name = folder;
    made.architectures = std::move(architectures);
    made.location = location;
    return made;
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

static const library* by_folder(const std::vector<library>& libraries, const std::string& folder) {
    for (const library& found : libraries) {
        if (found.folder_name == folder) {
            return &found;
        }
    }

    return nullptr;
}

static void check_details(const std::string& root) {
    std::string core = join_path(root, "details");
    write_tree(core, { "Network/src/Network.h", "Network/src/client/Client.cpp", "Network/src/Network.cpp",
                       "Wire/Wire.h", "Wire/Wire.cpp", "Wire/utility/twi.c", "Wire/utility/twi.h", "Old/Old.h",
                       "Old/arch/esp32/x.cpp", "Legacy/Legacy.h", "Legacy/src/x.cpp", "Both/src/Both.h",
                       "Both/utility/u.c" });
    write_text(join_path(core, "Both/library.properties"), "name=Both\nversion=1.0\n");
    write_text(join_path(core, "Network/library.properties"),
               "name=Networking\nversion=3.3.12\narchitectures=esp32, esp8266\ndot_a_linkage= true \n");
    write_text(join_path(core, "Old/library.properties"), "name=Old\nversion=1.0\nprecompiled=true\n");

    directory_layout layout = {};
    std::vector<library> libraries;
    std::string error;
    check(find_libraries(core, layout, libraries, error) && libraries.size() == 5, "the details' libraries load");

    const library* network = by_folder(libraries, "Network");
    const library* wire = by_folder(libraries, "Wire");
    const library* old = by_folder(libraries, "Old");
    const library* legacy = by_folder(libraries, "Legacy");
    const library* both = by_folder(libraries, "Both");
    if (!network || !wire || !old || !legacy || !both) {
        return;
    }

    check(legacy->layout == library_layout::flat && legacy->headers == std::vector<std::string>({ "Legacy.h" }),
          "a legacy library stays flat even with a src/ folder");
    check(both->layout == library_layout::recursive && both->utility_directory.empty(),
          "a src/ library takes no utility/ folder");

    check(network->layout == library_layout::recursive && network->dot_a_linkage, "a src/ library, linked as one");
    check(network->architectures == std::vector<std::string>({ "esp32", "esp8266" }), "its architectures, trimmed");
    check(network->headers == std::vector<std::string>({ "Network.h" }), "its headers directly in src/ only");

    check(wire->layout == library_layout::flat && wire->architectures == std::vector<std::string>({ "*" }),
          "a legacy library is flat and for every architecture");
    check(wire->utility_directory == join_path(core, "Wire/utility") && !wire->dot_a_linkage, "with its utility/");
    check(old->refusal == "keeps an arch folder, which libraries may no longer have", "an arch folder refuses it");
    check(library_source_directory(*network) == join_path(core, "Network/src") &&
              library_source_directory(*wire) == join_path(core, "Wire") &&
              library_build_directory("/b", *network) == "/b/libraries/Network",
          "a library's source folder, and its folder in a build tree named for its folder");

    std::vector<std::string> sources;
    check(library_sources(*network, sources, error) &&
              sources == std::vector<std::string>({ join_path(core, "Network/src/Network.cpp"),
                                                    join_path(core, "Network/src/client/Client.cpp") }),
          "a src/ library compiles src/ recursively");
    check(library_sources(*wire, sources, error) &&
              sources == std::vector<std::string>({ join_path(core, "Wire/Wire.cpp"),
                                                    join_path(core, "Wire/utility/twi.c") }),
          "a flat library compiles its folder, then utility/");
    check(library_include_directories(*wire, true) ==
              std::vector<std::string>({ join_path(core, "Wire"), join_path(core, "Wire/utility") }),
          "a flat library's own files also see utility/");
    check(libraries_providing(libraries, "Network.h") == std::vector<const library*>({ network }) &&
              libraries_providing(libraries, "twi.h").empty(),
          "a header in utility/ provides nothing");
}

static void check_ranking() {
    std::vector<std::string> esp32 = { "esp32" };
    std::vector<std::string> any = { "*" };

    check(library_priority("WiFi", "WiFi", esp32, library_location::core, "WiFi.h", "esp32") == 1010 + 700 + 2,
          "listing the architecture, the name and folder matching, the core's copy");
    check(library_priority("WiFiNINA", "WiFiNINA", any, library_location::installed, "WiFi.h", "esp32") ==
              1000 + 300 + 3,
          "any architecture, a name starting with the header, an installed library");
    check(library_priority("Servo", "Servo-main", { "avr" }, library_location::installed, "servo.h", "esp32") ==
              0 + 600 + 3,
          "another architecture, and names compared without case");

    check(library_priority("Foo Bar", "foo-main", any, library_location::installed, "Foo.h", "esp32") ==
              1000 + 500 + 3,
          "a folder named for the header and -main");
    check(library_priority("Big Display", "Big_Display", any, library_location::installed, "Display.h", "esp32") ==
              1000 + 200 + 3,
          "a name ending with the header, spaces made underscores");
    check(library_priority("A", "sub", any, library_location::installed, "sub.dir/x", "esp32") == 1000 + 3,
          "a dot in a header's folder is no extension, so the whole name is compared");
    check(library_priority("Bare", "Bare", {}, library_location::installed, "Bare.h", "esp32") == 1000 + 700 + 3,
          "a library naming no architecture ranks as one for any");

    check(library_priority("Other", "Thing", any, library_location::installed, "Thing.h", "esp32") == 1000 + 600 + 3,
          "the folder alone matching the header");
    check(library_priority("thing-main", "x", any, library_location::installed, "Thing.h", "esp32") == 1000 + 500 + 3,
          "the name alone ending in -main");
    check(library_priority("x", "thing-master", any, library_location::installed, "Thing.h", "esp32") ==
              1000 + 400 + 3,
          "the folder ending in -master, below -main");
    check(library_priority("x", "ThingBox", any, library_location::installed, "Thing.h", "esp32") == 1000 + 300 + 3,
          "the folder alone starting with the header");
    check(library_priority("x", "BigThing", any, library_location::installed, "Thing.h", "esp32") == 1000 + 200 + 3,
          "the folder alone ending with it");
    check(library_priority("x", "AThingB", any, library_location::installed, "Thing.h", "esp32") == 1000 + 100 + 3,
          "the folder alone holding it");

    library zeta = ranked("Zeta", "Zeta", any, library_location::installed);
    library alpha = ranked("Alpha", "Alpha", any, library_location::installed);
    library listed = ranked("Gamma", "Gamma", esp32, library_location::core);
    check(pick_library({ &zeta, &alpha }, "Thing.h", "esp32") == &alpha &&
              pick_library({ &alpha, &zeta }, "Thing.h", "esp32") == &alpha,
          "equal ranks go to the first name in either order");
    check(pick_library({ &zeta, &alpha, &listed }, "Thing.h", "esp32") == &listed, "a better rank wins");
}

// A library that cannot load is skipped, as arduino-cli's libraries manager skips it, and so is a hidden folder
static void check_skipped(const std::string& root) {
    std::string folder = join_path(root, "skipped");
    write_tree(folder, { "Good/src/Good.h", "Broken/Broken.h", "Locked/Locked.h", ".hidden/Hidden.h", "Bare/Bare.h",
                         "Upper/src/Upper.h", "FlatLink/FlatLink.h", "Pre/src/Pre.h", "Full/src/Full.h",
                         "Kept/Kept.h", "Kept/arch/x.cpp", "Wide/src/Wide.hh" });

    write_text(join_path(folder, "Good/library.properties"), "name=Good\nversion=1.0\n");
    write_text(join_path(folder, "Broken/library.properties"), "name Broken\n");
    write_text(join_path(folder, "Locked/library.properties"), "name=Locked\n");
    write_text(join_path(folder, "Bare/library.properties"), "version=1.0\n");
    write_text(join_path(folder, "Upper/library.properties"), "name=Upper\ndot_a_linkage=True\n");

    write_text(join_path(folder, "FlatLink/library.properties"), "name=FlatLink\ndot_a_linkage=true\n");
    write_text(join_path(folder, "Pre/library.properties"), "name=Pre\nprecompiled=true\n");
    write_text(join_path(folder, "Full/library.properties"), "name=Full\nprecompiled=full\n");
    write_text(join_path(folder, "Wide/library.properties"), "name=Wide\n");
    chmod(join_path(folder, "Locked/library.properties").c_str(), 0);

    directory_layout layout = {};
    std::vector<library> libraries;
    std::string error;
    check(find_libraries(folder, layout, libraries, error), "a broken library does not fail the others: " + error);
    check(by_folder(libraries, "Good") && !by_folder(libraries, "Broken") && !by_folder(libraries, "Locked") &&
              !by_folder(libraries, ".hidden"),
          "a malformed or unreadable library.properties and a hidden folder are skipped");

    const library* bare = by_folder(libraries, "Bare");
    const library* upper = by_folder(libraries, "Upper");
    const library* flat = by_folder(libraries, "FlatLink");
    const library* kept = by_folder(libraries, "Kept");
    const library* wide = by_folder(libraries, "Wide");

    check(bare && bare->name == "-", "a library.properties without a name names the library -, as arduino-cli does");
    check(upper && !upper->dot_a_linkage && flat && !flat->dot_a_linkage,
          "dot_a_linkage takes exactly true, and only a src/ library");
    check(by_folder(libraries, "Pre") && by_folder(libraries, "Full") &&
              !by_folder(libraries, "Pre")->refusal.empty() && !by_folder(libraries, "Full")->refusal.empty(),
          "precompiled true and full are both refused");

    check(kept && kept->refusal.empty(), "a legacy library with an arch folder is not refused");
    check(wide && libraries_providing(libraries, "Wide.hh") == std::vector<const library*>({ wide }),
          "a .hh header provides its library");
    chmod(join_path(folder, "Locked/library.properties").c_str(), 0644);
}

static void check_architectures() {
    check(board_architectures("esp32", "") == std::vector<std::string>({ "esp32" }) &&
              board_architectures("esp32", "esp8266,avr") == std::vector<std::string>({ "esp32", "esp8266", "avr" }),
          "the board's architecture, then those architecture.override_check adds");

    library servo = ranked("Servo", "Servo", { "avr", "megaavr" }, library_location::installed);
    library any = ranked("Any", "Any", { "*" }, library_location::installed);
    library listed = ranked("Listed", "Listed", { "esp32" }, library_location::installed);
    check(incompatible_libraries({ &servo, &any, &listed }, { "esp32" }) == std::vector<const library*>({ &servo }),
          "only a library listing none of the architectures is incompatible");
    check(incompatible_libraries({ &servo }, { "esp32", "avr" }).empty(), "an accepted architecture makes it fit");
}

int main() {
    char root_template[] = "/tmp/mcufw-libraries-XXXXXX";
    std::string root = mkdtemp(root_template);
    mkdir(join_path(root, "core").c_str(), 0755);
    mkdir(join_path(root, "libraries").c_str(), 0755);

    check_folder_names();
    check_found(root);
    check_details(root);
    check_ranking();
    check_skipped(root);
    check_architectures();

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
