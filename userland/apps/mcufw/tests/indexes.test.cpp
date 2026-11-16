/* indexes, run on the build machine against an excerpt of Arduino's package index: a core's releases with their
 * archives and tools, which tools are data, and an index that is cut short */

#include "indexes.hpp"

#include "files.hpp"

#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;
constexpr uint64_t INDEX_INFLATED_BYTES = 256 << 20;
constexpr size_t INDEX_PACKAGES = 256;
constexpr size_t INDEX_TOOLS = 4096;

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

static void write_gzip(const std::string& path, const std::string& text) {
    gzFile file = gzopen(path.c_str(), "wb");
    gzwrite(file, text.data(), static_cast<unsigned>(text.size()));
    gzclose(file);
}

/* An index whose array holds only spaces, `size` bytes inflated in all, written without holding it whole */
static void write_spaced_index(const std::string& path, uint64_t size) {
    const std::string head = "{\"packages\": [";
    const std::string tail = "]}";
    std::string spaces(1 << 20, ' ');
    gzFile file = gzopen(path.c_str(), "wb1");
    gzwrite(file, head.data(), static_cast<unsigned>(head.size()));

    uint64_t left = size - head.size() - tail.size();
    while (left > 0) {
        unsigned piece = static_cast<unsigned>(std::min<uint64_t>(left, spaces.size()));
        gzwrite(file, spaces.data(), piece);
        left -= piece;
    }

    gzwrite(file, tail.data(), static_cast<unsigned>(tail.size()));
    gzclose(file);
}

/* An index of `packages` packages, the first holding `tools` tools */
static std::string counted_index(size_t packages, size_t tools) {
    std::string text = "{\"packages\": [";
    for (size_t p = 0; p < packages; p++) {
        text += (p == 0 ? "" : ",") + std::string("{\"name\": \"p") + std::to_string(p) + "\", \"tools\": [";
        for (size_t t = 0; p == 0 && t < tools; t++) {
            text += (t == 0 ? "" : ",") + std::string("{\"name\": \"t") + std::to_string(t) + "\"}";
        }

        text += "]}";
    }

    return text + "]}";
}

static bool reads(const std::string& path, std::string& error) {
    std::vector<core_release> cores;
    std::vector<tool_release> tools;

    return read_package_index(path, "esp32", "esp32", cores, tools, error);
}

static void check_bounds(const std::string& root) {
    std::string index = join_path(root, "bounded.json.gz");
    std::string error;
    write_spaced_index(index, INDEX_INFLATED_BYTES);
    check(reads(index, error), "an index inflating to exactly 256 MiB reads");
    write_spaced_index(index, INDEX_INFLATED_BYTES + 1);
    check(!reads(index, error) &&
              error == "cannot read the package index, since " + index + " inflates to more than 256 MiB",
          "an index inflating one byte past 256 MiB fails, as a gzip bomb would");

    write_gzip(index, counted_index(INDEX_PACKAGES, 0));
    check(reads(index, error), "an index of 256 packages reads");
    write_gzip(index, counted_index(INDEX_PACKAGES + 1, 0));
    check(!reads(index, error) &&
              error == "cannot read the package index, since the index lists more than 256 packages",
          "an index of 257 packages fails");

    write_gzip(index, counted_index(1, INDEX_TOOLS));
    check(reads(index, error), "an index of 4096 tools reads");
    write_gzip(index, counted_index(1, INDEX_TOOLS + 1));
    check(!reads(index, error) &&
              error == "cannot read the package index, since the index lists more than 4096 tools",
          "an index of 4097 tools fails");
}

static const tool_release* find_tool(const std::vector<tool_release>& tools, const std::string& name) {
    for (const tool_release& tool : tools) {
        if (tool.name == name) {
            return &tool;
        }
    }

    return nullptr;
}

static void check_releases(const std::vector<core_release>& cores) {
    check(cores.size() == 2 && cores[0].version == "3.3.12" && cores[1].version == "3.3.11",
          "both releases read, in the index's order");
    if (cores.size() != 2) {
        return;
    }

    const core_release& release = cores[0];
    check(release.packager == "esp32" && release.architecture == "esp32" && release.name == "esp32", "its names");
    check(release.archive.file_name == "esp32-core-3.3.12.zip" && release.archive.size == 51102895, "its archive");
    check(release.archive.checksum ==
          "SHA-256:87401004f279a206781e368b66098a8383b7cf87cce7811a6afd0163503fa78f", "its checksum as written");
    check(release.tools.size() == 4 && release.tools[0].packager == "arduino" && release.tools[0].name == "dfu-util",
          "its tools in order, another packager's among them");
    check(release.tools.size() == 4 && release.tools[1].name == "esp-x32" && release.tools[1].version == "2601",
          "each with its version");
}

static void check_tools(const std::vector<tool_release>& tools) {
    const tool_release* cmsis = find_tool(tools, "CMSIS");
    check(cmsis && cmsis->is_data && cmsis->archive.file_name == "CMSIS-4.5.0.tar.bz2", "host all makes data");

    const tool_release* libraries = find_tool(tools, "esp32-libs");
    check(libraries && libraries->is_data && libraries->archive.file_name == "esp32-libs-3.3.12.zip",
          "one archive for every host makes data");
    check(libraries && libraries->archive.size == 48020175, "a size written as a number");

    const tool_release* compiler = find_tool(tools, "esp-x32");
    check(compiler && !compiler->is_data && compiler->archive.url.empty(), "an archive per host is a program");

    const tool_release* esptool = find_tool(tools, "esptool_py");
    check(esptool && !esptool->is_data, "two hosts sharing one archive among others make no data");

    const tool_release* windows_only = find_tool(tools, "rl78");
    check(windows_only && !windows_only->is_data, "one host's archive alone makes no data");

    const tool_release* flasher = find_tool(tools, "windows-flasher");
    check(flasher && !flasher->is_data, "hosts sharing one archive make no data without a Linux host among them");

    const tool_release* oversized = find_tool(tools, "oversized");
    check(oversized && oversized->is_data && oversized->archive.size == 0, "a size past 64 bits reads as unstated");
}

int main() {
    char root_template[] = "/tmp/mcufw-indexes-XXXXXX";
    std::string root = mkdtemp(root_template);
    std::string text;
    std::string error;
    check(read_file("fixtures/package_index.json", text, error), "the excerpt reads");

    std::string index = join_path(root, "package_index.json.gz");
    write_gzip(index, text);

    std::vector<core_release> cores;
    std::vector<tool_release> tools;
    check(read_package_index(index, "esp32", "esp32", cores, tools, error), "the index reads");
    check_releases(cores);
    check_tools(tools);

    check(read_package_index(index, "esp32", "nosuch", cores, tools, error) && cores.empty(), "no such core");

    std::string cut = join_path(root, "cut.json.gz");
    write_gzip(cut, text.substr(0, text.size() / 2));
    check(!read_package_index(cut, "esp32", "esp32", cores, tools, error), "an index cut short fails");
    check(error == "cannot read the package index, since the document ends early", "and says so");

    check_bounds(root);

    std::string cleanup = "rm -rf " + root;
    if (system(cleanup.c_str()) != 0) {
        printf("indexes: could not delete %s\n", root.c_str());
    }

    if (g_failures != 0) {
        printf("indexes: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("indexes: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
