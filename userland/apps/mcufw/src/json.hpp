#ifndef MCUFW_JSON_HPP
#define MCUFW_JSON_HPP

// A reader for JSON documents that are one object holding one large array, as Arduino's indexes are. It is fed bytes as
// they arrive and hands over each element of that array parsed whole, so it holds one element at a time.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr size_t JSON_DEPTH_MAX   = 64;
constexpr size_t JSON_ELEMENT_MAX = 16 << 20;  // bytes of one element as written
constexpr size_t JSON_VALUES_MAX  = 1 << 20;   // values in one element, which bounds its parsed size

enum class json_kind { null, boolean, number, string, array, object };

struct json_value {
    json_kind                                       kind = json_kind::null;
    std::string                                     text;     // a string, a number as written, `true` or `false`
    std::vector<json_value>                         items;    // an array's
    std::vector<std::pair<std::string, json_value>> members;  // an object's, in order
};

// The member `name` of object `value`, or nullptr when it has none
const json_value* json_member(const json_value& value, std::string_view name);

// Where the reader is in the document, defined in json.cpp
struct json_stream;

// Returning false stops the reader
using json_element_handler = std::function<bool(json_value& element, std::string& error)>;

class json_array_reader {
public:
    // Hands each element of the array that the top-level object holds under `array_key` to `take`. The object's other
    // members are parsed and dropped.
    json_array_reader(std::string array_key, json_element_handler take);
    ~json_array_reader();

    // Fails on malformed JSON or text after the document, an element past one of the caps above, or a handler that
    // returned false
    bool feed(std::string_view bytes, std::string& error);

    // Fails unless the top-level object closed
    bool finish(std::string& error);

private:
    std::unique_ptr<json_stream> m_stream;
};

#endif // MCUFW_JSON_HPP
