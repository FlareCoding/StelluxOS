/* json, run on the build machine: elements handed over whole however the bytes arrive, the other members dropped,
 * strings and numbers as RFC 8259 writes them, and the failures and caps */

#include "json.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

constexpr const char* DOCUMENT =
    "{ \"name\": \"index\", \"packages\": [\n"
    "  { \"name\": \"esp32\", \"size\": \"51102895\", \"tags\": [true, false, null] },\n"
    "  { \"name\": \"caf\\u00e9 \\ud83d\\ude00\", \"escapes\": \"\\\"\\\\\\/\\b\\f\\n\\r\\t\", \"number\": -1.5e+3,\n"
    "    \"path\": \"C:\\\\\" }\n"
    "], \"other\": { \"nested\": [1, 2, { \"deep\": [] }] } }\n";

/* A document the reader refuses, with the reason it gives */
struct malformed {
    const char* document;
    const char* error;
};

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

// Feeds `document` in pieces of `piece` bytes and keeps every element
static bool read_document(const std::string& document, size_t piece, std::vector<json_value>& elements,
                          std::string& error) {
    elements.clear();
    json_array_reader reader("packages", [&](json_value& element, std::string&) {
        elements.push_back(element);
        return true;
    });

    for (size_t start = 0; start < document.size(); start += piece) {
        if (!reader.feed(std::string_view(document).substr(start, piece), error)) {
            return false;
        }
    }

    return reader.finish(error);
}

static std::string text_of(const json_value& object, const char* name) {
    const json_value* member = json_member(object, name);
    return member ? member->text : "<none>";
}

static void check_elements() {
    for (size_t piece : { size_t(1), size_t(7), size_t(4096) }) {
        std::vector<json_value> elements;
        std::string error;
        check(read_document(DOCUMENT, piece, elements, error), "the document reads in any pieces");
        check(elements.size() == 2, "each element is handed over once");
        if (elements.size() != 2) {
            continue;
        }

        check(text_of(elements[0], "name") == "esp32" && text_of(elements[0], "size") == "51102895", "strings");

        const json_value* tags = json_member(elements[0], "tags");
        check(tags && tags->items.size() == 3 && tags->items[0].text == "true" &&
              tags->items[2].kind == json_kind::null, "literals");

        check(text_of(elements[1], "name") == "caf\xc3\xa9 \xf0\x9f\x98\x80", "escapes become UTF-8, pairs joined");
        check(text_of(elements[1], "escapes") == "\"\\/\b\f\n\r\t", "every short escape");
        check(text_of(elements[1], "number") == "-1.5e+3", "a number keeps its text");
        check(text_of(elements[1], "path") == "C:\\", "a string can end in an escaped backslash");
    }
}

static bool fails(const std::string& document, const std::string& expected_error) {
    std::vector<json_value> elements;
    std::string error;
    return !read_document(document, 1, elements, error) && error == expected_error;
}

static void check_failures() {
    const malformed documents[] = {
        { "{\"packages\": [1, 2,]}", "the document is malformed" },
        { "{\"packages\": {}}", "`packages` is no array" },
        { "{\"packages\": [01]}", "a value has text after it" },
        { "{\"packages\": [1.]}", "a number is malformed" },
        { "{\"packages\": [\"a\nb\"]}", "a string holds a control character" },
        { "{\"packages\": [\"\\ud800\"]}", "a \\u escape names no character" },
        { "{\"packages\": [nul]}", "a value is malformed" },
        { "{\"packages\": []} x", "the document has text after it" },
        { "{\"packages\": [", "the document ends early" },
        { "[1]", "the document is malformed" },
    };

    for (const malformed& expected : documents) {
        check(fails(expected.document, expected.error), expected.document);
    }
}

static std::string document_of(const std::string& element) {
    return "{\"packages\": [" + element + "]}";
}

static std::string nested_arrays(size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
}

static std::string array_of_zeros(size_t count) {
    std::string array = "[";
    for (size_t i = 0; i < count; i++) {
        array += i == 0 ? "0" : ",0";
    }

    return array + "]";
}

/* Each cap holds an element exactly at it and refuses one a step past it */
static void check_caps() {
    std::vector<json_value> elements;
    std::string error;
    check(read_document(document_of(nested_arrays(JSON_DEPTH_MAX)), 4096, elements, error),
          "an element nested exactly to the depth cap reads");
    check(fails(document_of(nested_arrays(JSON_DEPTH_MAX + 1)), "values nest deeper than 64"),
          "an element nested one level past the cap fails");

    std::string at_cap = "\"" + std::string(JSON_ELEMENT_MAX - 2, 'x') + "\"";
    check(read_document(document_of(at_cap), 1 << 20, elements, error) && elements.size() == 1,
          "an element exactly as long as the cap reads");
    std::string past_cap = "\"" + std::string(JSON_ELEMENT_MAX - 1, 'x') + "\"";
    check(!read_document(document_of(past_cap), 1 << 20, elements, error) &&
              error == "a value is longer than 16 MiB",
          "an element one byte past the cap fails");

    check(read_document(document_of(array_of_zeros(JSON_VALUES_MAX - 1)), 1 << 20, elements, error),
          "an element holding exactly the most values reads");
    check(!read_document(document_of(array_of_zeros(JSON_VALUES_MAX)), 1 << 20, elements, error) &&
              error == "an element holds more than 1048576 values",
          "an element holding one value more fails");
}

static void check_handler_stop() {
    json_array_reader reader("packages", [](json_value&, std::string& error) {
        error = "the handler stopped";
        return false;
    });

    std::string error;
    check(!reader.feed("{\"packages\": [1, 2]}", error) && error == "the handler stopped", "a handler can stop it");

    std::vector<json_value> elements;
    check(read_document("{\"packages\": []}", 1, elements, error) && elements.empty(), "an empty array is fine");
    check(read_document("{}", 1, elements, error) && elements.empty(), "so is an object without the array");
}

int main() {
    check_elements();
    check_failures();
    check_caps();
    check_handler_stop();

    if (g_failures != 0) {
        printf("json: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("json: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
