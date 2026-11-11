#ifndef STLXDM_SHELL_QUICK_MATCH_HPP
#define STLXDM_SHELL_QUICK_MATCH_HPP

#include <cstdint>
#include <string_view>
#include <vector>

/* How well a query finds a row, best first: a prefix of its name, a prefix of a word of its name or of a keyword,
 * a run inside its name, and the query's letters in order through its name */
enum class match_quality : uint8_t { prefix, word, substring, letters, none };

/* Bytes of the name the query matched, which the palette marks */
struct match_run {
    uint32_t start;
    uint32_t length;
};

struct query_match {
    match_quality          quality = match_quality::none;
    std::vector<match_run> runs;
};

/* Matches `query` against a row's `name` and its space separated `keywords`, ignoring ASCII case. Letters in order
 * count from three letters on, and an empty query matches nothing. A keyword match marks no runs. */
query_match match_query(std::string_view query, std::string_view name, std::string_view keywords);

#endif
