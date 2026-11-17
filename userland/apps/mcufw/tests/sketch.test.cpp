/* sketch, run on the build machine on sketch folders it writes: which files compile, which are copied and which are
 * left out, the merge of the .ino files, and sketch.yaml's two keys read and saved */

#include "files.hpp"
#include "sketch.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
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

// Writes `text` at `path` below `root`, making the folders on the way
static void write_text(const std::string& root, const std::string& path, const std::string& text) {
    for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        mkdir(join_path(root, path.substr(0, slash)).c_str(), 0755);
    }

    FILE* file = fopen(join_path(root, path).c_str(), "w");
    fputs(text.c_str(), file);
    fclose(file);
}

static std::string read_text(const std::string& path) {
    std::string text;
    std::string error;
    return read_file(path, text, error) ? text : "<unreadable>";
}

static void write_lamp(const std::string& root) {
    write_text(root, "Lamp/Lamp.ino", "\xEF\xBB\xBFint level = 0;\nvoid setup() {}\n");
    write_text(root, "Lamp/Colors.ino", "void set_color(int color) {}\n");
    write_text(root, "Lamp/helpers.cpp", "void helper() {}\n");
    write_text(root, "Lamp/helpers.h", "void helper();\n");

    write_text(root, "Lamp/src/effects/fade.cpp", "void fade() {}\n");
    write_text(root, "Lamp/src/effects/fade.h", "void fade();\n");
    write_text(root, "Lamp/src/blend.c", "void blend(void) {}\n");
    write_text(root, "Lamp/data/notes.md", "Notes on the lamp.\n");
    write_text(root, "Lamp/extra/unused.cpp", "void unused() {}\n");

    write_text(root, "Lamp/extra/ignored.ino", "void ignored() {}\n");
    write_text(root, "Lamp/build/config.json", "{}\n");
    write_text(root, "Lamp/old/build.options.json", "{}\n");
    write_text(root, "Lamp/old/stale.cpp", "void stale() {}\n");
    write_text(root, "Lamp/.hidden.h", "\n");
    write_text(root, "Lamp/picture.png", "\n");

    write_text(root, "Lamp/zz.cpp", "void last() {}\n");
    write_text(root, "Lamp/.vscode/settings.json", "{}\n");
    write_text(root, "Lamp/.hidden/notes.h", "\xEF\xBB\xBFvoid note();\n");
}

static void check_loading(const std::string& root) {
    sketch found;
    std::string error;
    std::string folder = join_path(root, "Lamp");

    check(load_sketch(join_path(folder, "Lamp.ino"), found, error), "a sketch loads from its main file");
    check(found.folder == folder && found.name == "Lamp" && found.main_file == "Lamp.ino", "its folder and name");
    check(found.other_ino_files == std::vector<std::string>({ "Colors.ino" }), "only the folder's other .ino files");
    check(found.source_files ==
              std::vector<std::string>({ "helpers.cpp", "zz.cpp", "src/blend.c", "src/effects/fade.cpp" }),
          "the folder's sources by name, then src/'s by path");
    check(found.copied_files == std::vector<std::string>({ ".hidden/notes.h", ".vscode/settings.json", "data/notes.md",
                                                           "extra/unused.cpp", "helpers.h", "src/effects/fade.h" }),
          "the other files a build copies, dot folders walked, leaving out build/, build trees, dot files and others");

    sketch again;
    check(load_sketch(folder, again, error) && again.main_file == "Lamp.ino", "a sketch loads from its folder");

    check(load_sketch(join_path(root, "Glow"), found, error) && found.main_file == "Glow.pde", "a .pde main file");
    check(!load_sketch(join_path(root, "Twice"), found, error) && error.find("Twice.pde") != std::string::npos,
          "two main files fail");
    check(!load_sketch(join_path(root, "Empty"), found, error), "a folder without its main file fails");
    check(!load_sketch(join_path(root, "Missing"), found, error), "a missing folder fails");
}

static void check_merge(const std::string& root) {
    sketch found;
    merged_sketch merged;
    std::string error;
    std::string folder = join_path(root, "Lamp");
    check(load_sketch(folder, found, error) && merge_ino_files(found, merged, error), "the .ino files merge");
    check(merged.text == "#include <Arduino.h>\n"
                         "#line 1 \"" + folder + "/Lamp.ino\"\nint level = 0;\nvoid setup() {}\n\n"
                         "#line 1 \"" + folder + "/Colors.ino\"\nvoid set_color(int color) {}\n\n",
          "Arduino.h first, then each file after its mark, the byte order mark dropped");

    check(load_sketch(join_path(root, "Glow"), found, error) && merge_ino_files(found, merged, error),
          "a main file including Arduino.h merges");
    check(merged.text.rfind("#line 1 ", 0) == 0, "and gets no include of its own");

    for (const char* name : { "Later", "Quoted" }) {
        check(load_sketch(join_path(root, name), found, error) && merge_ino_files(found, merged, error) &&
                  merged.text.rfind("#line 1 ", 0) == 0,
              "a main file including Arduino.h on a later line, or in quotes, gets no include of its own");
    }

    std::string quoted_folder = join_path(root, "Say\"Hi");
    check(load_sketch(quoted_folder, found, error) && merge_ino_files(found, merged, error) &&
              merged.text.find("#line 1 \"" + join_path(root, "Say\\\"Hi/Say\\\"Hi.ino") + "\"\n") != std::string::npos,
          "a quote in the path is escaped in the mark, as cpp.QuoteString writes it");
}

static void check_copies(const std::string& root) {
    sketch found;
    std::string text;
    std::string error;
    std::string folder = join_path(root, "Lamp");
    check(load_sketch(folder, found, error), "the sketch loads");

    check(copied_file_text(found, ".hidden/notes.h", text, error) &&
              text == "#line 1 \"" + folder + "/.hidden/notes.h\"\nvoid note();\n",
          "a copied file loses its byte order mark and gains a mark naming the original");
    check(copied_file_text(found, "helpers.cpp", text, error) &&
              text == "#line 1 \"" + folder + "/helpers.cpp\"\nvoid helper() {}\n",
          "a copied file without a byte order mark keeps its text");
}

static void check_links(const std::string& root) {
    sketch found;
    std::string error;
    check(!load_sketch(join_path(root, "Loop"), found, error) && error.find("symbolic link loop") != std::string::npos,
          "a symbolic link back to a folder holding it fails, as arduino-cli's walk does");
    check(load_sketch(join_path(root, "Twin"), found, error) &&
              std::find(found.copied_files.begin(), found.copied_files.end(), "linked/blend.c") !=
                  found.copied_files.end(),
          "a link to another folder is walked: " + error);
}

static void check_settings(const std::string& root) {
    sketch found;
    sketch_settings settings;
    std::string error;
    load_sketch(join_path(root, "Lamp"), found, error);
    check(read_sketch_settings(found, settings, error) && settings.fqbn.empty() && settings.port.empty(),
          "a sketch without sketch.yaml has no settings");

    std::string yaml = join_path(found.folder, "sketch.yaml");
    write_text(root, "Lamp/sketch.yaml", "profiles:\n  esp32:\n    fqbn: esp32:esp32:esp32\n"
                                         "default_fqbn: \"esp32:esp32:esp32da:FlashSize=8M\"\n"
                                         "default_port: /dev/serial/cp2102-0001 # the desk board\n");
    check(read_sketch_settings(found, settings, error) && settings.fqbn == "esp32:esp32:esp32da:FlashSize=8M" &&
              settings.port == "/dev/serial/cp2102-0001",
          "the two keys read, unquoted and without their comment");

    check(save_sketch_fqbn(found, "esp32:esp32:esp32s3", error), "saving the board replaces its line");
    check(read_text(yaml) == "profiles:\n  esp32:\n    fqbn: esp32:esp32:esp32\n"
                             "default_fqbn: esp32:esp32:esp32s3\n"
                             "default_port: /dev/serial/cp2102-0001 # the desk board\n",
          "and leaves every other line as it was");

    write_text(root, "Lamp/sketch.yaml", "default_port: /dev/serial/ch340-0001");
    check(save_sketch_fqbn(found, "esp32:esp32:esp32da", error) &&
              read_text(yaml) == "default_port: /dev/serial/ch340-0001\ndefault_fqbn: esp32:esp32:esp32da\n",
          "saving a board that sketch.yaml lacks adds its line");

    unlink(yaml.c_str());
    check(save_sketch_fqbn(found, "esp32:esp32:esp32da", error) &&
              read_text(yaml) == "default_fqbn: esp32:esp32:esp32da\n",
          "saving a board creates sketch.yaml");
}

int main() {
    char root_template[] = "/tmp/mcufw-sketch-XXXXXX";
    char* made = mkdtemp(root_template);
    char resolved[PATH_MAX];
    std::string root = realpath(made, resolved);

    write_lamp(root);
    write_text(root, "Glow/Glow.pde", "#include <Arduino.h>\nvoid setup() {}\n");
    write_text(root, "Twice/Twice.ino", "\n");
    write_text(root, "Twice/Twice.pde", "\n");
    write_text(root, "Empty/notes.md", "\n");

    write_text(root, "Later/Later.ino", "// The lamp's later sketch\n#include <Arduino.h>\nvoid setup() {}\n");
    write_text(root, "Quoted/Quoted.ino", "#include \"Arduino.h\"\nvoid setup() {}\n");
    write_text(root, "Say\"Hi/Say\"Hi.ino", "void setup() {}\n");
    write_text(root, "Loop/Loop.ino", "void setup() {}\n");
    write_text(root, "Twin/Twin.ino", "void setup() {}\n");

    bool linked = symlink(".", join_path(root, "Loop/self").c_str()) == 0 &&
                  symlink("../Lamp/src", join_path(root, "Twin/linked").c_str()) == 0;
    check(linked, "the links are made");

    check_loading(root);
    check_merge(root);
    check_copies(root);
    check_links(root);
    check_settings(root);

    std::string cleanup = "rm -rf " + root;
    if (system(cleanup.c_str()) != 0) {
        printf("sketch: could not delete %s\n", root.c_str());
    }

    if (g_failures != 0) {
        printf("sketch: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("sketch: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
