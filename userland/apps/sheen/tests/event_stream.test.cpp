/* event_line_stream and split_event_line, run on the build machine: verifies SPACE_STLXPKG, TSV_MCUFW, and LINES
 * framing across arbitrary byte chunks, CRLF endings, trailing flushes, and free-text failure reasons. */

#include "workspace/event_stream.h"

#include <cstdint>
#include <cstdio>
#include <string>
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

static void test_stlxpkg_protocol() {
    std::vector<std::vector<std::string>> events;
    sheen::event_line_stream stream(sheen::event_protocol::SPACE_STLXPKG);

    const std::string payload =
        "start python 3.12.8-2 41943040\n"
        "progress python 20971520 41943040\r\n"
        "\n"
        "   \r\n"
        "commit python\n"
        "installed python 3.12.8-2 98412000\n"
        "failed esptool conflict esptool cannot install /usr/bin/python3, which is already there\n";

    stream.feed(payload, [&events](const std::vector<std::string>& fields) { events.push_back(fields); });

    check(events.size() == 5, "stlxpkg emits five non-empty events");
    if (events.size() == 5) {
        check(events[0] == std::vector<std::string>{ "start", "python", "3.12.8-2", "41943040" },
              "stlxpkg start fields");
        check(events[1] == std::vector<std::string>{ "progress", "python", "20971520", "41943040" },
              "stlxpkg progress strips CRLF");
        check(events[2] == std::vector<std::string>{ "commit", "python" }, "stlxpkg commit fields");
        check(events[3] == std::vector<std::string>{ "installed", "python", "3.12.8-2", "98412000" },
              "stlxpkg installed fields");
        check(events[4] == std::vector<std::string>{
                  "failed", "esptool", "conflict",
                  "esptool cannot install /usr/bin/python3, which is already there" },
              "stlxpkg failed keeps remainder reason intact");
    }
}

static void test_mcufw_tsv_protocol() {
    std::vector<std::vector<std::string>> events;
    sheen::event_line_stream stream(sheen::event_protocol::TSV_MCUFW);

    const std::string payload =
        "board\tesp32:esp32:esp32da\t-\tESP32-WROOM-DA Module\n"
        "problem\terror\t/sketches/Blink/Blink.ino\t1\t10\tFastLED.h: No such file or directory\r\n"
        "hint\tFastLED.h\tFastLED\t3.10.9\n"
        "log\tcompile\t\n"
        "size\tflash\t880980\t1310720\n"
        "failed\tcompile\t1 error in Blink.ino\n";

    stream.feed(payload, [&events](const std::vector<std::string>& fields) { events.push_back(fields); });

    check(events.size() == 6, "mcufw TSV emits six events");
    if (events.size() == 6) {
        check(events[0] == std::vector<std::string>{ "board", "esp32:esp32:esp32da", "-", "ESP32-WROOM-DA Module" },
              "mcufw board TSV fields");
        check(events[1] == std::vector<std::string>{
                  "problem", "error", "/sketches/Blink/Blink.ino", "1", "10",
                  "FastLED.h: No such file or directory" },
              "mcufw problem TSV fields");
        check(events[2] == std::vector<std::string>{ "hint", "FastLED.h", "FastLED", "3.10.9" },
              "mcufw hint TSV fields");
        check(events[3] == std::vector<std::string>{ "log", "compile", "" },
              "mcufw log preserves empty trailing TSV field");
        check(events[4] == std::vector<std::string>{ "size", "flash", "880980", "1310720" },
              "mcufw size TSV fields");
        check(events[5] == std::vector<std::string>{ "failed", "compile", "1 error in Blink.ino" },
              "mcufw failed TSV fields");
    }
}

static void test_lines_protocol() {
    std::vector<std::vector<std::string>> events;
    sheen::event_line_stream stream(sheen::event_protocol::LINES);

    stream.feed("first line with   spaces\r\n\nsecond\tline\n",
                [&events](const std::vector<std::string>& fields) { events.push_back(fields); });

    check(events.size() == 2, "LINES emits two non-empty lines");
    if (events.size() == 2) {
        check(events[0] == std::vector<std::string>{ "first line with   spaces" },
              "LINES keeps spaces and strips CRLF");
        check(events[1] == std::vector<std::string>{ "second\tline" }, "LINES keeps tabs intact");
    }
}

static void test_byte_by_byte_chunk_splits_and_flush() {
    std::vector<std::vector<std::string>> events;
    sheen::event_line_stream stream(sheen::event_protocol::TSV_MCUFW);

    const std::string wire =
        "open\tcp2102-0001\t115200\r\n"
        "gone\tcp2102-0001\r\n"
        "back\tcp2102-0001\n"
        "closed\tcp2102-0001";

    for (char ch : wire) {
        stream.feed(std::string_view(&ch, 1), [&events](const std::vector<std::string>& fields) {
            events.push_back(fields);
        });
    }

    check(events.size() == 3, "partial unterminated tail waits for newline or flush");
    stream.flush([&events](const std::vector<std::string>& fields) { events.push_back(fields); });
    check(events.size() == 4, "flush emits trailing unterminated event");
    if (events.size() == 4) {
        check(events[0] == std::vector<std::string>{ "open", "cp2102-0001", "115200" }, "byte-split CRLF open");
        check(events[1] == std::vector<std::string>{ "gone", "cp2102-0001" }, "byte-split CRLF gone");
        check(events[2] == std::vector<std::string>{ "back", "cp2102-0001" }, "byte-split LF back");
        check(events[3] == std::vector<std::string>{ "closed", "cp2102-0001" }, "flushed closed");
    }

    stream.flush([&events](const std::vector<std::string>& fields) { events.push_back(fields); });
    check(events.size() == 4, "second flush on empty buffer emits nothing");
}

static void test_none_protocol_ignores_chunks() {
    std::vector<std::vector<std::string>> events;
    sheen::event_line_stream stream(sheen::event_protocol::NONE);
    stream.feed("start python 3.12\n", [&events](const std::vector<std::string>& fields) {
        events.push_back(fields);
    });
    stream.flush([&events](const std::vector<std::string>& fields) { events.push_back(fields); });
    check(events.empty(), "NONE protocol emits zero events");
}

int main() {
    test_stlxpkg_protocol();
    test_mcufw_tsv_protocol();
    test_lines_protocol();
    test_byte_by_byte_chunk_splits_and_flush();
    test_none_protocol_ignores_chunks();

    if (g_failures != 0) {
        std::printf("event_stream: %llu of %llu checks failed\n",
                    static_cast<unsigned long long>(g_failures),
                    static_cast<unsigned long long>(g_checks));
        return 1;
    }

    std::printf("event_stream: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
