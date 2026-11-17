#include "prototypes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// ctags reads a string literal or a character literal as one character above ASCII, which lands in signatures as is
constexpr int STRING_MARK = 'S' + 0x80;
constexpr int CHAR_MARK   = 'C' + 0x80;
constexpr int END_OF_TEXT = -1;

// ctags remembers a statement's last 15 tokens, which bounds how far back a return type reaches
constexpr size_t STATEMENT_TOKENS = 15;

constexpr size_t PUSHED_BACK_MAX = 2;

constexpr std::string_view EXTERN_C = "extern \"C\"";

// arduino-cli's filter of the preprocessed sketch reads lines into a 1 MiB buffer and stops at a longer one
constexpr size_t SCANNED_LINE_MAX = 1024 * 1024;

// arduino-cli's checks read the .ino files with a bufio.Scanner's default buffer of 64 KiB
constexpr size_t FILE_LINE_MAX = 64 * 1024;

namespace {

// A line of the scanned text, numbered as the preprocessor's markers number it
struct scanned_line {
    std::string_view text;
    std::string      file;
    uint32_t         number;
};

enum class token_type {
    none, args, brace_close, brace_open, colon, comma, double_colon, keyword, name, paren_name, semicolon, star,
    ampersand,
};

struct scan_token {
    token_type  type = token_type::none;
    std::string text;
    size_t      line = 0;  // the scanned line the token was read on
};

// How a statement declares its names
enum class storage { plain, file_static, external, friend_declaration, type_alias };

enum class declaration {
    none, base, class_type, enum_type, function, ignored, namespace_scope, extern_c_block, struct_type, union_type,
};

// The C++ parser of Arduino's patched ctags 5.8, the one arduino-cli runs, ported from its c.c and get.c with the
// originals' function names and mistakes kept, since arduino-cli's prototypes depend on them
struct statement {
    const statement*                         parent = nullptr;
    storage                                  storage_kind = storage::plain;
    declaration                              declared = declaration::none;
    bool                                     got_name = false;
    bool                                     have_qualifying_name = false;  // a name follows a `*` or `::`
    bool                                     got_paren_name = false;        // a name in parentheses, as `(*fn)`
    bool                                     got_args = false;
    bool                                     is_pointer = false;
    bool                                     in_function = false;
    bool                                     assignment = false;
    bool                                     not_variable = false;  // a `.`, `->` or call rules one out
    bool                                     is_virtual = false;
    size_t                                   token_index = 0;
    std::array<scan_token, STATEMENT_TOKENS> tokens;
    scan_token                               context;     // the qualification `A::B` a name was read with
    scan_token                               block_name;  // the class, struct or namespace the statement opens
};

struct parenthesized_group {
    bool is_pointer = false;
    bool is_param_list = true;
    bool is_name_candidate = true;
    bool invalid_contents = false;
    bool nested_args = false;
};

enum class tag_kind { function, prototype, other };

// A tag as arduino-cli reads ctags' output
struct found_tag {
    std::string name;
    tag_kind    kind = tag_kind::other;
    bool        scoped = false;   // ctags names a class, struct or namespace around it
    std::string file;
    int64_t     line = 0;
    std::string code;             // its line, escaped as ctags writes a search pattern
    std::string prototype;        // the return type, the name and the signature, then `;`
    std::string modifiers;
    bool        skipped = false;
};

struct scanner {
    std::vector<scanned_line>             lines;
    std::string                           text;  // the scanned lines, each ending in a newline
    size_t                                position = 0;
    size_t                                line = 0;  // the line of the character read last
    bool                                  line_ended = false;
    bool                                  directive_allowed = true;
    std::array<int, PUSHED_BACK_MAX>      pushed_back = {};
    size_t                                pushed_count = 0;
    bool                                  collecting_signature = false;
    std::string                           signature;
    std::string                           return_type;
    bool                                  brace_format = false;  // ctags' second pass, after a brace error
    bool                                  brace_error = false;
    bool                                  stopped = false;  // the text ended, or a bracket never closed
    std::vector<found_tag>                tags;
};

// Each .ino file's lines as arduino-cli's checks read them from disk, by the path its line markers name
using file_texts = std::map<std::string, std::vector<std::string>>;

} // anonymous namespace

static bool is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static bool is_identifier_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '~' || c == '$';
}

static bool is_identifier_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

static bool is_one_of(int c, std::string_view set) {
    return c > 0 && c < 0x100 && set.find(static_cast<char>(c)) != std::string_view::npos;
}

static bool is_cpp_keyword(std::string_view word) {
    static const std::unordered_set<std::string_view> keywords = {
        "__attribute__", "case", "catch", "char", "class", "const", "constexpr", "default", "delete", "do", "double",
        "else", "enum", "explicit", "extern", "float", "for", "friend", "goto", "if", "inline", "int", "long",
        "mutable", "namespace", "new", "noexcept", "operator", "overload", "private", "protected", "public",
        "register", "return", "short", "signed", "static", "static_assert", "struct", "switch", "template", "this",
        "throw", "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
        "wchar_t", "while",
    };

    return keywords.contains(word);
}

static bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

static std::string without_spaces_and_tabs(std::string_view text) {
    std::string kept;
    for (char c : text) {
        if (c != ' ' && c != '\t') {
            kept += c;
        }
    }

    return kept;
}

// A quoted string as the preprocessor writes it in a line marker, with backslash escapes
static bool parse_marker_string(std::string_view text, std::string& value, std::string_view& rest) {
    if (text.empty() || text[0] != '"') {
        return false;
    }

    value.clear();
    for (size_t i = 1; i < text.size(); i++) {
        if (text[i] == '\\') {
            i++;
            if (i >= text.size()) {
                return false;
            }

            value += text[i];
        } else if (text[i] == '"') {
            rest = text.substr(i + 1);
            return true;
        } else {
            value += text[i];
        }
    }

    return false;
}

// A number as Go's `strconv.Atoi` reads one: an optional sign, then decimal digits, within a signed 64-bit range
static bool is_signed_decimal(std::string_view text) {
    bool negative = !text.empty() && text[0] == '-';
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
        text.remove_prefix(1);
    }

    bool digits = !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    std::string_view largest = negative ? "9223372036854775808" : "9223372036854775807";

    return digits && (text.size() < largest.size() || (text.size() == largest.size() && text <= largest));
}

// A marker as arduino-cli recognizes one when it filters the preprocessor's output: a word starting with `#`, a
// number and a quoted file name, separated by single spaces
static bool read_arduino_line_marker(std::string_view line, std::string& file) {
    size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0 || line[0] != '#') {
        return false;
    }

    size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return false;
    }

    if (!is_signed_decimal(line.substr(first_space + 1, second_space - first_space - 1))) {
        return false;
    }

    std::string_view rest;
    if (!parse_marker_string(line.substr(second_space + 1), file, rest)) {
        return false;
    }

    return rest.empty() || rest[0] == ' ';
}

// A marker as ctags reads one under `--line-directives`. It takes the file name up to its closing quote without
// unescaping, and arduino-cli then turns each `\\` back into `\`.
static bool read_ctags_line_directive(std::string_view line, uint32_t& number, std::string& file) {
    size_t i = 1;
    auto skip_blanks = [&]() {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            i++;
        }
    };

    if (line.empty() || line[0] != '#') {
        return false;
    }

    skip_blanks();
    if (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
    } else if (line.substr(i, 4) == "line" && i + 4 < line.size() && (line[i + 4] == ' ' || line[i + 4] == '\t')) {
        i += 5;
    } else {
        return false;
    }

    skip_blanks();
    uint64_t value = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        value = value * 10 + static_cast<uint64_t>(line[i] - '0');
        i++;
    }

    if (i >= line.size() || (line[i] != ' ' && line[i] != '\t') || value == 0) {
        return false;
    }

    skip_blanks();
    std::string name;
    if (i < line.size() && line[i] == '"') {
        size_t close = line.find('"', i + 1);
        name = line.substr(i + 1, close == std::string_view::npos ? std::string_view::npos : close - i - 1);
    } else {
        size_t end = line.find_first_of(" \t", i);
        name = line.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
    }

    file.clear();
    for (size_t j = 0; j < name.size(); j++) {
        file += name[j];
        if (name[j] == '\\' && j + 1 < name.size() && name[j + 1] == '\\') {
            j++;
        }
    }

    number = static_cast<uint32_t>(value);
    return true;
}

static void load_lines(scanner& s, std::string_view lines) {
    std::string file;
    uint32_t number = 1;
    size_t start = 0;
    while (start < lines.size()) {
        size_t end = lines.find('\n', start);
        if (end == std::string_view::npos) {
            end = lines.size();
        }

        std::string_view line = lines.substr(start, end - start);
        start = end + 1;

        uint32_t marked_number = 0;
        std::string marked_file;
        if (read_ctags_line_directive(line, marked_number, marked_file)) {
            if (!marked_file.empty()) {
                file = marked_file;
            }

            number = marked_number;
            continue;
        }

        s.lines.push_back({line, file, number});
        s.text += line;
        s.text += '\n';
        number++;
    }
}

static int peek_character(const scanner& s) {
    return s.position < s.text.size() ? static_cast<unsigned char>(s.text[s.position]) : END_OF_TEXT;
}

static int read_character(scanner& s) {
    if (s.position >= s.text.size()) {
        return END_OF_TEXT;
    }

    if (s.line_ended) {
        s.line++;
        s.line_ended = false;
    }

    char c = s.text[s.position++];
    if (c == '\n') {
        s.line_ended = true;
    }

    return static_cast<unsigned char>(c);
}

static void unread_character(scanner& s) {
    s.position--;
    if (s.text[s.position] == '\n') {
        s.line_ended = false;
    }
}

static void skip_c_comment(scanner& s) {
    int previous = 0;
    int c;
    while ((c = read_character(s)) != END_OF_TEXT) {
        if (previous == '*' && c == '/') {
            return;
        }

        previous = c;
    }
}

// Leaves the comment's newline to be read next
static void skip_cpp_comment(scanner& s) {
    int c;
    while ((c = peek_character(s)) != END_OF_TEXT && c != '\n') {
        read_character(s);
        if (c == '\\') {
            read_character(s);
        }
    }
}

static int skip_string(scanner& s) {
    int c;
    while ((c = read_character(s)) != END_OF_TEXT) {
        if (c == '\\') {
            read_character(s);
        } else if (c == '"') {
            break;
        }
    }

    return STRING_MARK;
}

// ctags also reads Vera's based numbers, such as 'b1010, in the same pass
static int skip_character_literal(scanner& s) {
    int c;
    int count = 0;
    int base = 0;
    while ((c = read_character(s)) != END_OF_TEXT) {
        count++;
        if (c == '\\') {
            read_character(s);
        } else if (c == '\'') {
            break;
        } else if (c == '\n') {
            unread_character(s);
            break;
        } else if (count == 1 && is_one_of(std::toupper(c), "DHOB")) {
            base = c;
        } else if (base != 0 && !std::isalnum(c)) {
            unread_character(s);
            break;
        }
    }

    return CHAR_MARK;
}

static void push_back(scanner& s, int c) {
    if (s.pushed_count == PUSHED_BACK_MAX) {
        s.pushed_back[0] = s.pushed_back[1];
        s.pushed_count--;
    }

    s.pushed_back[s.pushed_count++] = c;
}

// The next character for the parser: comments read as white space, literals as one mark each, and preprocessor
// lines skipped whole
static int next_character(scanner& s) {
    if (s.pushed_count > 0) {
        return s.pushed_back[--s.pushed_count];
    }

    bool in_directive = false;
    while (true) {
        int c = read_character(s);
        switch (c) {
        case END_OF_TEXT:
            return c;
        case ' ':
        case '\t':
            break;
        case '\n':
            in_directive = false;
            s.directive_allowed = true;
            break;
        case '"':
            s.directive_allowed = false;
            c = skip_string(s);
            break;
        case '\'':
            s.directive_allowed = false;
            c = skip_character_literal(s);
            break;
        case '#':
            if (s.directive_allowed) {
                in_directive = true;
                s.directive_allowed = false;
            }

            break;
        case '/':
            if (peek_character(s) == '*') {
                read_character(s);
                skip_c_comment(s);
                c = ' ';
            } else if (peek_character(s) == '/') {
                read_character(s);
                skip_cpp_comment(s);
                continue;
            } else {
                s.directive_allowed = false;
            }

            break;
        case '\\':
            if (peek_character(s) == '\n') {
                read_character(s);
                continue;
            }

            break;
        default:
            s.directive_allowed = false;
            break;
        }

        if (!in_directive) {
            return c;
        }
    }
}

// The next character that is not white space. A run of white space reads into a signature as one space.
static int skip_white(scanner& s) {
    bool found = false;
    int c;
    while (is_space(c = next_character(s))) {
        found = true;
    }

    if (s.collecting_signature && found) {
        s.signature += ' ';
    }

    return c;
}

static int skip_to_one_of(scanner& s, std::string_view chars) {
    int c;
    do {
        c = next_character(s);
    } while (c != END_OF_TEXT && c != 0 && !is_one_of(c, chars));
    return c;
}

static void stop_at_unmatched(scanner& s, int begin) {
    s.stopped = true;
    if (begin == '{') {
        s.brace_error = true;
    }
}

// Skips to the character that closes `pair`, whose opening is read. In a template's `<...>`, a `<<` opens nothing,
// and a `;` or `{` ends the skip.
static void skip_to_match(scanner& s, std::string_view pair) {
    const int begin = pair[0];
    const int end = pair[1];
    int level = 1;
    int c = 0;
    while (level > 0 && (c = skip_white(s)) != END_OF_TEXT) {
        if (s.collecting_signature) {
            s.signature += static_cast<char>(c);
        }

        if (c == begin) {
            int next = next_character(s);
            if (c == '<' && next == '<') {
                if (s.collecting_signature) {
                    s.signature += static_cast<char>(next);
                }
            } else {
                push_back(s, next);
                level++;
            }
        } else if (c == end) {
            push_back(s, next_character(s));
            level--;
        } else if (begin == '<' && (c == ';' || c == '{')) {
            push_back(s, c);
            break;
        }
    }

    if (c == END_OF_TEXT) {
        stop_at_unmatched(s, begin);
    }
}

static void skip_parens(scanner& s) {
    int c = skip_white(s);
    if (c == '(') {
        skip_to_match(s, "()");
    } else {
        push_back(s, c);
    }
}

static void skip_braces(scanner& s) {
    int c = skip_white(s);
    if (c == '{') {
        skip_to_match(s, "{}");
    } else {
        push_back(s, c);
    }
}

static scan_token& active_token(statement& st) {
    return st.tokens[st.token_index];
}

static scan_token& previous_token(statement& st, size_t count) {
    return st.tokens[(st.token_index + STATEMENT_TOKENS - count) % STATEMENT_TOKENS];
}

static void reset_token(const scanner& s, scan_token& token) {
    token.type = token_type::none;
    token.text.clear();
    token.line = s.line;
}

static void advance_token(const scanner& s, statement& st) {
    st.token_index = (st.token_index + 1) % STATEMENT_TOKENS;
    reset_token(s, active_token(st));
}

static void set_token(const scanner& s, statement& st, token_type type) {
    reset_token(s, active_token(st));
    active_token(st).type = type;
}

static void step_back_token(const scanner& s, statement& st) {
    st.token_index = (st.token_index + STATEMENT_TOKENS - 1) % STATEMENT_TOKENS;
    set_token(s, st, token_type::none);
}

static bool is_word(const scan_token& token, std::string_view word) {
    return token.type == token_type::keyword && token.text == word;
}

static bool is_contextual(const statement* st) {
    if (st == nullptr) {
        return false;
    }

    switch (st->declared) {
    case declaration::class_type:
    case declaration::enum_type:
    case declaration::namespace_scope:
    case declaration::struct_type:
    case declaration::union_type:
        return true;
    default:
        return false;
    }
}

static bool is_member(const statement& st) {
    return st.context.type == token_type::name || is_contextual(st.parent);
}

static bool inside_enum_body(const statement& st) {
    return st.parent != nullptr && st.parent->declared == declaration::enum_type;
}

static bool is_valid_type_specifier(declaration declared) {
    switch (declared) {
    case declaration::base:
    case declaration::class_type:
    case declaration::enum_type:
    case declaration::struct_type:
    case declaration::union_type:
        return true;
    default:
        return false;
    }
}

static void reinit_statement(const scanner& s, statement& st, bool partial) {
    if (!partial) {
        st.storage_kind = storage::plain;
        st.declared = is_contextual(st.parent) ? declaration::base : declaration::none;
    }

    st.got_paren_name = false;
    st.is_pointer = false;
    st.assignment = false;
    st.not_variable = false;
    st.is_virtual = false;

    st.got_args = false;
    st.got_name = false;
    st.have_qualifying_name = false;
    st.token_index = 0;
    st.in_function = st.parent != nullptr && st.parent->in_function;

    for (scan_token& token : st.tokens) {
        reset_token(s, token);
    }

    reset_token(s, st.context);
    if (!partial) {
        reset_token(s, st.block_name);
    }
}

// Whether ctags names a class, struct or namespace around the tag, which makes arduino-cli pass over it. ctags also
// names unions and enums, which arduino-cli does not read.
static bool is_scoped(const statement& st) {
    bool has_hierarchy = st.context.type == token_type::name && !st.context.text.empty();
    for (const statement* ancestor = st.parent; ancestor != nullptr && !has_hierarchy; ancestor = ancestor->parent) {
        has_hierarchy = is_contextual(ancestor);
    }

    bool parent_is_namespace = st.parent != nullptr && st.parent->declared == declaration::namespace_scope;
    if (!has_hierarchy || !(is_member(st) || parent_is_namespace)) {
        return false;
    }

    if (st.context.type == token_type::name) {
        return true;
    }

    declaration parent_declared = st.parent != nullptr ? st.parent->declared : declaration::none;
    return parent_declared == declaration::class_type || parent_declared == declaration::struct_type ||
           parent_declared == declaration::namespace_scope;
}

// The line as ctags writes it into a search pattern, `\` and `/` escaped, and a `$` too when it ends the line
static std::string pattern_line(std::string_view line) {
    std::string escaped;
    for (size_t i = 0; i < line.size() && line[i] != '\r'; i++) {
        bool ends_line = i + 1 == line.size() || line[i + 1] == '\r';
        if (line[i] == '\\' || line[i] == '/' || (line[i] == '$' && ends_line)) {
            escaped += '\\';
        }

        escaped += line[i];
    }

    return escaped;
}

static void make_tag(scanner& s, const statement& st, const scan_token& token, tag_kind kind) {
    if (token.type != token_type::name || token.text.empty()) {
        return;
    }

    const scanned_line& line = s.lines[token.line];
    found_tag tag;
    tag.name = token.text;
    tag.kind = kind;
    tag.scoped = is_scoped(st);

    tag.file = line.file;
    tag.line = line.number;
    tag.code = pattern_line(line.text);
    if (kind == tag_kind::other) {
        tag.prototype = " " + token.text + ";";
    } else {
        tag.prototype = s.return_type + " " + token.text + s.signature + ";";
    }

    s.tags.push_back(tag);
}

static void qualify_variable_tag(scanner& s, const statement& st, const scan_token& name) {
    if (name.type != token_type::name || st.storage_kind == storage::type_alias ||
        !is_valid_type_specifier(st.declared) || st.not_variable || is_member(st)) {
        return;
    }

    if (st.storage_kind == storage::external || !st.have_qualifying_name || st.in_function) {
        return;
    }

    make_tag(s, st, name, tag_kind::other);
}

static void qualify_function_declaration_tag(scanner& s, const statement& st, const scan_token& name) {
    if (name.type == token_type::name && st.storage_kind != storage::type_alias &&
        is_valid_type_specifier(st.declared)) {
        make_tag(s, st, name, tag_kind::prototype);
    }
}

static void process_token(scanner& s, statement& st, scan_token& token);

static void process_name(statement& st) {
    if (st.got_name && st.declared == declaration::none) {
        st.declared = declaration::base;
    }

    st.got_name = true;
    st.have_qualifying_name = true;
}

static void read_identifier(scanner& s, scan_token& token, int first) {
    reset_token(s, token);

    int c = first;
    if (first == '~') {
        token.text += '~';
        c = skip_white(s);
    }

    bool is_first = true;
    do {
        token.text += static_cast<char>(c);
        if (s.collecting_signature) {
            if (!is_first) {
                s.signature += static_cast<char>(c);
            }

            is_first = false;
        }

        c = next_character(s);
    } while (is_identifier_char(c));
    push_back(s, c);

    token.type = is_cpp_keyword(token.text) ? token_type::keyword : token_type::name;
}

static void read_operator(scanner& s, statement& st) {
    constexpr std::string_view operator_chars = "+-*/%^&|~!=<>,[]";
    const scan_token& previous = previous_token(st, 1);
    scan_token& token = active_token(st);

    int c = skip_white(s);
    if (is_word(previous, "enum") || is_word(previous, "struct") || is_word(previous, "union")) {
    } else if (c == '(') {
        if (next_character(s) == ')') {
            token.text += ' ';
            c = skip_white(s);
            if (c == '(') {
                token.text += "()";
            }
        } else {
            skip_to_match(s, "()");
            c = next_character(s);
        }
    } else if (is_identifier_start(c)) {
        bool white = true;
        do {
            if (is_space(c)) {
                white = true;
            } else {
                if (white) {
                    token.text += ' ';
                    white = false;
                }

                token.text += static_cast<char>(c);
            }

            c = next_character(s);
        } while (!is_one_of(c, "(;") && c != END_OF_TEXT);
    } else if (is_one_of(c, operator_chars)) {
        token.text += ' ';
        do {
            token.text += static_cast<char>(c);
            c = next_character(s);
        } while (is_one_of(c, operator_chars));
    }

    push_back(s, c);

    token.type = token_type::name;
    process_name(st);
}

static void skip_statement(scanner& s, statement& st) {
    st.declared = declaration::ignored;
    skip_to_one_of(s, ";");
}

static void set_access(scanner& s, statement& st) {
    if (!is_member(st)) {
        return;
    }

    int c = skip_white(s);
    if (c == ':') {
        reinit_statement(s, st, false);
    } else {
        push_back(s, c);
    }
}

static void process_token(scanner& s, statement& st, scan_token& token) {
    if (token.type == token_type::name) {
        process_name(st);
        return;
    }

    static const std::set<std::string_view> base_types = {
        "char", "const", "constexpr", "double", "float", "int", "long", "short", "signed", "unsigned", "void",
        "volatile", "wchar_t",
    };

    const std::string word = token.text;
    if (word == "__attribute__" || word == "typename" || word == "inline") {
        skip_parens(s);
        reset_token(s, token);
    } else if (word == "catch") {
        skip_parens(s);
        skip_braces(s);
    } else if (base_types.contains(word)) {
        st.declared = declaration::base;
    } else if (word == "class" || word == "struct") {
        if (st.declared != declaration::enum_type) {
            st.declared = word == "class" ? declaration::class_type : declaration::struct_type;
        }
    } else if (word == "enum") {
        st.declared = declaration::enum_type;
    } else if (word == "union") {
        st.declared = declaration::union_type;
    } else if (word == "friend") {
        st.storage_kind = storage::friend_declaration;
    } else if (word == "goto" || word == "return" || word == "using") {
        skip_statement(s, st);
    } else if (word == "namespace") {
        st.declared = declaration::namespace_scope;
    } else if (word == "operator") {
        read_operator(s, st);
    } else if (word == "private" || word == "protected" || word == "public") {
        set_access(s, st);
    } else if (word == "static_assert") {
        skip_parens(s);
    } else if (word == "typedef") {
        reinit_statement(s, st, false);
        st.storage_kind = storage::type_alias;
    } else if (word == "extern" || word == "static") {
        reinit_statement(s, st, false);
        st.storage_kind = word == "extern" ? storage::external : storage::file_static;
        st.declared = declaration::base;
    } else if (word == "virtual") {
        st.is_virtual = true;
    } else if (word == "for" || word == "if" || word == "switch" || word == "while") {
        if (skip_white(s) == '(') {
            skip_to_match(s, "()");
        }
    }
}

static void restart_statement(scanner& s, statement& st) {
    scan_token saved = active_token(st);
    reinit_statement(s, st, false);
    active_token(st) = saved;
    process_token(s, st, active_token(st));
}

static void skip_member_initializers(scanner& s, scan_token& token) {
    int c;
    do {
        c = skip_white(s);
        while (is_identifier_start(c) || c == ':') {
            if (c != ':') {
                read_identifier(s, token, c);
            }

            c = skip_white(s);
        }

        if (c == '<') {
            skip_to_match(s, "<>");
            c = skip_white(s);
        }

        if (c == '(') {
            skip_to_match(s, "()");
            c = skip_white(s);
        }
    } while (c == ',');
    push_back(s, c);
}

static void skip_macro(scanner& s, statement& st) {
    if (previous_token(st, 2).type == token_type::name) {
        step_back_token(s, st);
    }

    skip_to_match(s, "()");
}

static void process_angle_bracket(scanner& s) {
    int c = next_character(s);
    if (c == '>') {
    } else if (c != '<' && c != '=') {
        push_back(s, c);
        skip_to_match(s, "<>");
    } else if (c == '<') {
        c = next_character(s);
        if (c != '=') {
            push_back(s, c);
        }
    } else {
        push_back(s, c);
    }
}

// What follows a parameter list before its body or the end of its declaration: qualifiers, a throw specification,
// attributes or a constructor's member initializers. Any other word restarts the statement, `noexcept` among them.
static bool skip_after_parameter_list(scanner& s, statement& st) {
    scan_token& token = active_token(st);
    bool restart = false;
    bool end = false;
    int c = skip_white(s);
    do {
        switch (c) {
        case ')':
            break;
        case ':':
            skip_member_initializers(s, token);
            break;
        case '[':
            skip_to_match(s, "[]");
            break;
        case '=':
        case '{':
        case '}':
        case ';':
            push_back(s, c);
            end = true;
            break;
        case '(':
            skip_to_match(s, "()");
            break;
        default:
            if (!is_identifier_start(c)) {
                break;
            }

            read_identifier(s, token, c);
            if (is_word(token, "__attribute__") || is_word(token, "throw")) {
                skip_parens(s);
            } else if (is_word(token, "try")) {
            } else if (is_word(token, "const") || is_word(token, "volatile")) {
                if (!s.signature.empty()) {
                    s.signature += ' ';
                    s.signature += token.text;
                }
            } else if (token.type == token_type::name && (token.text == "override" || token.text == "final")) {
            } else {
                restart = true;
                end = true;
            }

            break;
        }

        if (!end) {
            c = skip_white(s);
            end = c == END_OF_TEXT;
        }
    } while (!end);

    if (restart) {
        restart_statement(s, st);
    } else {
        set_token(s, st, token_type::none);
    }

    return c != END_OF_TEXT;
}

static void analyze_post_parens(scanner& s, statement& st) {
    int c = skip_white(s);
    push_back(s, c);
    if (is_one_of(c, "{;,=")) {
        return;
    }

    if (!skip_after_parameter_list(s, st)) {
        s.stopped = true;
    }
}

// The return type of the function whose name the token before `(` holds: the statement's tokens before the name, its
// qualification left out, keywords followed by a space, and `*`, `&` and `::` written as they are
static void parse_return_type(scanner& s, statement& st) {
    s.return_type.clear();
    if (previous_token(st, 1).type == token_type::none) {
        return;
    }

    size_t lower = 1;
    if (previous_token(st, 2).type == token_type::double_colon) {
        size_t colons = 0;
        for (size_t j = 2; j < STATEMENT_TOKENS && previous_token(st, j).type == token_type::double_colon; j += 2) {
            colons++;
        }

        lower = 2 * colons + 1;
    }

    size_t upper = STATEMENT_TOKENS - 1;
    for (size_t i = 0; i < STATEMENT_TOKENS; i++) {
        token_type type = previous_token(st, i).type;
        if (type == token_type::brace_close || type == token_type::brace_open) {
            upper = i == 0 ? STATEMENT_TOKENS - 1 : i - 1;
            break;
        }
    }

    for (size_t i = upper; i > lower; i--) {
        const scan_token& token = previous_token(st, i);
        switch (token.type) {
        case token_type::paren_name:
        case token_type::none:
            break;
        case token_type::double_colon:
            s.return_type += "::";
            break;
        case token_type::star:
            s.return_type += '*';
            break;
        case token_type::ampersand:
            s.return_type += '&';
            break;
        default:
            s.return_type += token.text;
            if (token.type == token_type::keyword) {
                s.return_type += ' ';
            }

            break;
        }
    }

    size_t first = s.return_type.find_first_not_of(" \t\n\v\f\r");
    size_t last = s.return_type.find_last_not_of(" \t\n\v\f\r");
    s.return_type = first == std::string::npos ? "" : s.return_type.substr(first, last - first + 1);
}

static void parse_parens(scanner& s, statement& st, parenthesized_group& info) {
    scan_token& token = active_token(st);
    unsigned identifiers = 0;
    unsigned depth = 1;
    bool first = true;

    s.collecting_signature = true;
    s.signature = "(";
    do {
        int c = skip_white(s);
        s.signature += static_cast<char>(c);

        switch (c) {
        case '&':
        case '*':
            info.is_pointer = true;
            if (identifiers == 0) {
                info.is_param_list = false;
            }

            reset_token(s, token);
            break;
        case '.':
            info.is_name_candidate = false;
            c = next_character(s);
            if (c != '.') {
                push_back(s, c);
            } else if ((c = next_character(s)) != '.') {
                push_back(s, c);
            } else {
                s.signature += "...";
            }

            break;
        case ',':
        case '=':
            info.is_name_candidate = false;
            if (c == '=' && first) {
                info.is_param_list = false;
                skip_macro(s, st);
                depth = 0;
            }

            break;
        case '[':
            skip_to_match(s, "[]");
            break;
        case '<':
            process_angle_bracket(s);
            break;
        case ')':
            depth--;
            break;
        case '(':
            if (first) {
                info.is_name_candidate = false;
                push_back(s, c);
                s.signature.clear();
                skip_macro(s, st);
                depth = 0;
                if (!s.signature.empty()) {
                    s.signature.pop_back();
                }
            } else if (token.type == token_type::paren_name) {
                c = skip_white(s);
                if (c == '*') {
                    skip_to_match(s, "()");
                    c = skip_white(s);
                    if (c == '(') {
                        skip_to_match(s, "()");
                    } else {
                        push_back(s, c);
                    }
                } else {
                    push_back(s, c);
                    push_back(s, '(');
                    info.nested_args = true;
                }
            } else {
                depth++;
            }

            break;
        case ':':
            break;
        default:
            if (is_identifier_start(c)) {
                identifiers++;
                read_identifier(s, token, c);
                if (token.type == token_type::name && info.is_name_candidate) {
                    token.type = token_type::paren_name;
                } else if (token.type == token_type::keyword && token.text != "const" && token.text != "volatile") {
                    info.is_name_candidate = false;
                }
            } else {
                info.is_param_list = false;
                info.is_name_candidate = false;
                info.invalid_contents = true;
            }

            break;
        }

        first = false;
    } while (!info.nested_args && depth > 0 && info.is_name_candidate && !s.stopped);

    while (!info.nested_args && depth > 0 && !s.stopped) {
        skip_to_match(s, "()");
        depth--;
    }

    if (!info.is_name_candidate) {
        reset_token(s, token);
    }

    s.collecting_signature = false;
}

static void analyze_parens(scanner& s, statement& st) {
    if (st.in_function && !st.assignment) {
        st.not_variable = true;
    }

    if (previous_token(st, 1).type == token_type::none) {
        return;
    }

    scan_token& token = active_token(st);
    parenthesized_group info;
    parse_parens(s, st, info);
    parse_return_type(s, st);

    int c = skip_white(s);
    push_back(s, c);
    if (info.invalid_contents) {
        reinit_statement(s, st, false);
    } else if (info.is_name_candidate && token.type == token_type::paren_name && !st.got_paren_name &&
               (!info.is_param_list || !st.have_qualifying_name || c == '(' || (c == '=' && !st.is_virtual) ||
                (st.declared == declaration::none && is_one_of(c, ",;")))) {
        token.type = token_type::name;
        process_name(st);
        st.got_paren_name = true;
        if (!(c == '(' && info.nested_args)) {
            st.is_pointer = info.is_pointer;
        }
    } else if (!st.got_args && info.is_param_list) {
        st.got_args = true;
        set_token(s, st, token_type::args);
        advance_token(s, st);
        if (st.storage_kind != storage::type_alias) {
            analyze_post_parens(s, st);
        }
    } else {
        set_token(s, st, token_type::none);
    }
}

static void add_context(statement& st, const scan_token& token) {
    if (token.type != token_type::name) {
        return;
    }

    if (!st.context.text.empty()) {
        st.context.text += "::";
    }

    st.context.text += token.text;
    st.context.type = token_type::name;
}

static void read_parents(scanner& s) {
    int c;
    do {
        c = skip_white(s);
        if (is_identifier_start(c)) {
            scan_token ignored;
            read_identifier(s, ignored, c);
        } else if (c == '<') {
            skip_to_match(s, "<>");
        }
    } while (c != '{' && c != END_OF_TEXT && !s.stopped);
    push_back(s, c);
}

static void process_colon(scanner& s, statement& st) {
    int c = next_character(s);
    if (c == ':') {
        set_token(s, st, token_type::double_colon);
        st.have_qualifying_name = false;
        return;
    }

    push_back(s, c);
    declaration parent_declared = st.parent != nullptr ? st.parent->declared : declaration::none;
    if (st.declared == declaration::class_type || st.declared == declaration::struct_type) {
        read_parents(s);
    } else if (parent_declared == declaration::struct_type) {
        c = skip_to_one_of(s, ",;");
        if (c == ',') {
            set_token(s, st, token_type::comma);
        } else if (c == ';') {
            set_token(s, st, token_type::semicolon);
        }
    } else if (st.declared == declaration::enum_type) {
        c = skip_to_one_of(s, "{;");
        if (c == '{') {
            set_token(s, st, token_type::brace_open);
        } else if (c == ';') {
            set_token(s, st, token_type::semicolon);
        }
    } else if (is_word(previous_token(st, 1), "default") || is_word(previous_token(st, 2), "case") ||
               st.parent != nullptr) {
        reinit_statement(s, st, false);
    }
}

static int skip_initializer(scanner& s, statement& st) {
    while (true) {
        int c = skip_white(s);
        switch (c) {
        case END_OF_TEXT:
            s.stopped = true;
            return c;
        case ',':
        case ';':
            return c;
        case '[':
            skip_to_match(s, "[]");
            break;
        case '(':
            skip_to_match(s, "()");
            break;
        case '{':
            skip_to_match(s, "{}");
            break;
        case '<':
            process_angle_bracket(s);
            break;
        case '}':
            if (inside_enum_body(st)) {
                return c;
            }

            if (!s.brace_format) {
                s.brace_error = true;
                s.stopped = true;
            }

            break;
        default:
            break;
        }

        if (s.stopped) {
            return END_OF_TEXT;
        }
    }
}

static void process_initializer(scanner& s, statement& st) {
    const bool in_enum_body = inside_enum_body(st);
    int c = next_character(s);
    if (c == '=') {
        return;
    }

    push_back(s, c);
    c = skip_initializer(s, st);
    st.assignment = true;
    if (c == ';') {
        set_token(s, st, token_type::semicolon);
    } else if (c == ',') {
        set_token(s, st, token_type::comma);
    } else if (c == '}' && in_enum_body) {
        push_back(s, c);
        set_token(s, st, token_type::comma);
    }

    if (st.storage_kind == storage::external) {
        st.storage_kind = storage::plain;
    }
}

static void parse_general_token(scanner& s, statement& st, int c) {
    const scan_token& previous = previous_token(st, 1);
    if (is_identifier_start(c)) {
        scan_token& token = active_token(st);
        read_identifier(s, token, c);
        process_token(s, st, token);
        if (st.context.type == token_type::name && active_token(st).type == token_type::name &&
            previous.type == token_type::name) {
            reset_token(s, st.context);
        }
    } else if (c == '.' || c == '-') {
        if (!st.assignment) {
            st.not_variable = true;
        }

        if (c == '-') {
            int next = next_character(s);
            if (next != '>') {
                push_back(s, next);
            }
        }
    } else if (c == '!' || c == '>') {
        int next = next_character(s);
        if (next != '=') {
            push_back(s, next);
        }
    } else if (c == STRING_MARK && !st.have_qualifying_name && st.storage_kind == storage::external) {
        st.declared = declaration::extern_c_block;
        st.storage_kind = storage::plain;
    }
}

static void next_token(scanner& s, statement& st) {
    do {
        int c = skip_white(s);
        switch (c) {
        case END_OF_TEXT:
            s.stopped = true;
            return;
        case '(':
            analyze_parens(s, st);
            break;
        case '<':
            process_angle_bracket(s);
            break;
        case '*':
            st.have_qualifying_name = false;
            set_token(s, st, token_type::star);
            break;
        case '&':
            set_token(s, st, token_type::ampersand);
            break;
        case ',':
            set_token(s, st, token_type::comma);
            break;
        case ':':
            process_colon(s, st);
            break;
        case ';':
            set_token(s, st, token_type::semicolon);
            break;
        case '=':
            process_initializer(s, st);
            break;
        case '[':
            skip_to_match(s, "[]");
            break;
        case '{':
            set_token(s, st, token_type::brace_open);
            break;
        case '}':
            set_token(s, st, token_type::brace_close);
            break;
        default:
            parse_general_token(s, st, c);
            break;
        }
    } while (!s.stopped && active_token(st).type == token_type::none);
}

// A function tag for a parameter list followed by `{`, and a prototype tag for one followed by `;` or `,`
static void tag_check(scanner& s, statement& st) {
    const scan_token& token = active_token(st);
    const scan_token& previous = previous_token(st, 1);
    const scan_token& before_previous = previous_token(st, 2);

    switch (token.type) {
    case token_type::brace_open:
        if (previous.type == token_type::args) {
            if (st.have_qualifying_name) {
                st.declared = declaration::function;
                if (before_previous.type == token_type::name) {
                    st.block_name = before_previous;
                }

                make_tag(s, st, before_previous, tag_kind::function);
            }
        } else if (is_contextual(&st)) {
            bool final_after_name = previous.text == "final" && before_previous.type == token_type::name;
            if (previous.type == token_type::name && final_after_name) {
                st.block_name = before_previous;
            } else if (previous.type == token_type::name) {
                st.block_name = previous;
            } else {
                st.block_name.type = token_type::name;
                st.block_name.text = "__anon";
            }

            if (st.declared == declaration::struct_type) {
                make_tag(s, st, previous, tag_kind::other);
            }
        }

        break;
    case token_type::semicolon:
    case token_type::comma:
        if (inside_enum_body(st)) {
        } else if (previous.type == token_type::name) {
            bool after_compound_keyword = is_word(before_previous, "class") || is_word(before_previous, "enum") ||
                                          is_word(before_previous, "namespace") || is_word(before_previous, "struct") ||
                                          is_word(before_previous, "union");
            if (!after_compound_keyword) {
                qualify_variable_tag(s, st, previous);
            }
        } else if (previous.type == token_type::args && before_previous.type == token_type::name) {
            if (st.is_pointer) {
                qualify_variable_tag(s, st, before_previous);
            } else {
                qualify_function_declaration_tag(s, st, before_previous);
            }
        }

        break;
    default:
        break;
    }
}

static void create_tags(scanner& s, size_t level, const statement* parent);

// Reads a class, struct, union, enum, namespace or `extern "C"` body as statements one level down, and skips any other
// block whole, a function's body among them
static void nest(scanner& s, statement& st, size_t level) {
    switch (st.declared) {
    case declaration::class_type:
    case declaration::enum_type:
    case declaration::namespace_scope:
    case declaration::extern_c_block:
    case declaration::struct_type:
    case declaration::union_type:
        create_tags(s, level, &st);
        break;
    case declaration::function:
        st.in_function = true;
        skip_to_match(s, "{}");
        break;
    default:
        skip_to_match(s, "{}");
        break;
    }

    advance_token(s, st);
    set_token(s, st, token_type::brace_close);
}

// A statement keeps its last 15 tokens until a `;`, a `,` or the `}` of a block other than a class, struct, union, enum
// or namespace ends it
static void check_statement_end(scanner& s, statement& st) {
    token_type type = active_token(st).type;
    if (type == token_type::comma) {
        reinit_statement(s, st, true);
    } else if (type == token_type::semicolon || (type == token_type::brace_close && !is_contextual(&st))) {
        reinit_statement(s, st, false);
    } else {
        advance_token(s, st);
    }
}

static void create_tags(scanner& s, size_t level, const statement* parent) {
    statement st;
    st.parent = parent;
    reinit_statement(s, st, false);

    while (!s.stopped) {
        next_token(s, st);
        if (s.stopped) {
            return;
        }

        token_type type = active_token(st).type;
        if (type == token_type::brace_close) {
            if (level > 0) {
                return;
            }

            s.brace_error = true;
            s.stopped = true;
            return;
        }

        if (type == token_type::double_colon) {
            add_context(st, previous_token(st, 1));
            advance_token(s, st);
            continue;
        }

        tag_check(s, st);
        if (type == token_type::brace_open) {
            nest(s, st, level + 1);
        }

        check_statement_end(s, st);
    }
}

// The tags ctags writes for `lines`. ctags reads a text again with its second brace rule after a brace error, and
// keeps what that pass found.
static std::vector<found_tag> scan_tags(std::string_view lines, std::vector<scanned_line>& scanned) {
    scanner first;
    load_lines(first, lines);
    create_tags(first, 0, nullptr);
    scanned = first.lines;
    if (!first.brace_error) {
        return first.tags;
    }

    scanner second;
    load_lines(second, lines);
    second.brace_format = true;
    create_tags(second, 0, nullptr);
    return second.tags;
}

// Each file's lines as arduino-cli's checks read them with a bufio.Scanner: split at `\n` with a final `\r` dropped,
// a byte order mark kept, and reading stopped at a line too long for the scanner's buffer
static file_texts texts_by_file(const std::vector<ino_text>& files) {
    file_texts texts;
    for (const ino_text& file : files) {
        std::vector<std::string>& lines = texts[file.path];
        size_t start = 0;
        while (start < file.text.size()) {
            size_t end = file.text.find('\n', start);
            if (end == std::string::npos) {
                end = file.text.size();
            }

            if (end - start >= FILE_LINE_MAX) {
                break;
            }

            std::string line = file.text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            lines.push_back(line);
            start = end + 1;
        }
    }

    return texts;
}

static const std::string& text_line(const std::vector<std::string>& text, int64_t number) {
    static const std::string missing;
    return number >= 1 && static_cast<uint64_t>(number) <= text.size() ? text[number - 1] : missing;
}

// arduino-cli's `removeComments`, mistakes kept: the `/` of `*/` stays and the line's last character goes, and a line
// with `*/` but no `/*` reads empty, as does every later line read up to one holding `/*`
static std::string remove_comments(std::string text, bool& in_comment) {
    size_t line_comment = text.find("//");
    if (line_comment != std::string::npos) {
        text.resize(line_comment);
    }

    size_t close = text.find("*/");
    if (close != std::string::npos) {
        size_t open = text.find("/*");
        std::string after = text.substr(close + 1, text.size() - close - 2);
        if (open != std::string::npos) {
            text = text.substr(0, open) + after;
        } else {
            text = after;
            in_comment = true;
        }
    }

    if (in_comment) {
        size_t open = text.find("/*");
        if (open != std::string::npos) {
            text.resize(open);
            in_comment = false;
        } else {
            text.clear();
        }
    }

    return text;
}

static std::string up_to_first_close_paren(const std::string& text) {
    size_t close = text.find(')');
    return close == std::string::npos ? "" : text.substr(0, close + 1);
}

// arduino-cli's template prototype: the definition's lines from the one holding `template` up to the parameter list
static std::string template_prototype(const found_tag& tag, const file_texts& texts) {
    std::string code = tag.code;
    if (starts_with(code, "template")) {
        size_t brace = code.find('{');
        size_t close = code.rfind(')');
        if (brace != std::string::npos) {
            return code.substr(0, brace) + ";";
        }

        return code.substr(0, close == std::string::npos ? 0 : close + 1) + ";";
    }

    auto found = texts.find(tag.file);
    if (found != texts.end()) {
        int64_t line = std::min<int64_t>(std::max<int64_t>(tag.line - 1, 0), found->second.size());
        bool in_comment = false;
        while (line > 0 && code.find("template") == std::string::npos) {
            line--;
            code = remove_comments(found->second[line], in_comment) + code;
        }
    }

    return up_to_first_close_paren(code) + ";";
}

static void add_prototypes(std::vector<found_tag>& tags, const file_texts& texts) {
    for (found_tag& tag : tags) {
        if (tag.skipped) {
            continue;
        }

        if (starts_with(tag.prototype, "template")) {
            tag.prototype = template_prototype(tag, texts);
            continue;
        }

        // arduino-cli adds `static` for any `static ` on the definition's line, a static local's included
        tag.modifiers = tag.code.find("static ") != std::string::npos ? "static" : "";
    }
}

static void remove_defined_prototypes(std::vector<found_tag>& tags) {
    std::set<std::string> declared;
    for (const found_tag& tag : tags) {
        if (tag.kind == tag_kind::prototype) {
            declared.insert(tag.prototype);
        }
    }

    for (found_tag& tag : tags) {
        if (declared.contains(tag.prototype)) {
            tag.skipped = true;
        }
    }
}

static void skip_duplicates(std::vector<found_tag>& tags) {
    std::set<std::string> seen;
    for (found_tag& tag : tags) {
        if (!tag.skipped && !seen.contains(tag.prototype)) {
            seen.insert(tag.prototype);
        } else {
            tag.skipped = true;
        }
    }
}

// arduino-cli's reach back for a definition whose name starts its line, taking earlier lines until the code is
// `missing` characters longer. `line` names the earliest line taken, counted from 0 as arduino-cli counts it.
static std::string with_earlier_lines(const found_tag& tag, std::string code, ptrdiff_t missing,
                                      const file_texts& texts, int64_t& line) {
    line = 0;
    auto found = texts.find(tag.file);
    if (found == texts.end()) {
        return code;
    }

    const ptrdiff_t expected = static_cast<ptrdiff_t>(code.size()) + missing;
    line = std::clamp<int64_t>(tag.line - 1, 0, static_cast<int64_t>(found->second.size()));
    bool in_comment = false;
    while (line > 0 && static_cast<ptrdiff_t>(code.size()) < expected) {
        line--;
        code = without_spaces_and_tabs(remove_comments(found->second[line], in_comment) + code);
    }

    return code;
}

// arduino-cli's check that the definition reads as the prototype ctags reported, reading on to the closing
// parenthesis or back over a return type on an earlier line, whose line the tag then takes
static bool prototype_and_code_differ(found_tag& tag, const file_texts& texts) {
    std::string code = without_spaces_and_tabs(tag.code);
    auto found = texts.find(tag.file);
    if (code.find(')') == std::string::npos && found != texts.end()) {
        bool in_comment = false;
        code = remove_comments(text_line(found->second, tag.line), in_comment);
        std::string next;
        for (int64_t number = tag.line + 1;
             number <= static_cast<int64_t>(found->second.size()) && next.find(')') == std::string::npos; number++) {
            next = remove_comments(text_line(found->second, number), in_comment);
            code += next;
        }
    }

    code = without_spaces_and_tabs(code);
    std::string prototype = without_spaces_and_tabs(tag.prototype);
    prototype.pop_back();
    if (code.find(prototype) != std::string::npos) {
        return false;
    }

    code = up_to_first_close_paren(code);
    size_t at = prototype.find(code);
    ptrdiff_t missing = at == std::string::npos ? -1 : static_cast<ptrdiff_t>(at);
    int64_t line = 0;
    code = with_earlier_lines(tag, code, missing, texts, line);
    if (code.find(prototype) == std::string::npos) {
        return true;
    }

    tag.line = line;
    return false;
}

// The lines arduino-cli counts as inside `extern "C"`. It scans the file of the first tag, then of each next tag
// until a tag's file was scanned already.
static std::map<std::string, std::vector<int64_t>> extern_c_lines(const std::vector<found_tag>& tags,
                                                                  const file_texts& texts) {
    const std::string marker = without_spaces_and_tabs(EXTERN_C);
    std::map<std::string, std::vector<int64_t>> lines;
    for (const found_tag& tag : tags) {
        if (lines.contains(tag.file)) {
            break;
        }

        auto found = texts.find(tag.file);
        if (found == texts.end()) {
            continue;
        }

        std::vector<int64_t>& inside = lines[tag.file];
        inside.push_back(-1);

        bool in_scope = false;
        bool entering = false;
        int64_t level = 0;
        int64_t number = 0;
        for (const std::string& text : found->second) {
            number++;
            std::string compact = without_spaces_and_tabs(text);
            if (compact.empty()) {
                continue;
            }

            entering = false;
            if (compact.find(marker) != std::string::npos) {
                in_scope = true;
                entering = compact.size() == marker.size();
            }

            if (in_scope) {
                inside.push_back(number);
            }

            level += std::count(compact.begin(), compact.end(), '{') - std::count(compact.begin(), compact.end(), '}');
            if (level == 0 && !entering) {
                in_scope = false;
            }
        }
    }

    return lines;
}

static void add_c_linkage(std::vector<found_tag>& tags, const file_texts& texts) {
    std::map<std::string, std::vector<int64_t>> inside = extern_c_lines(tags, texts);
    for (found_tag& tag : tags) {
        const std::vector<int64_t>& lines = inside[tag.file];
        bool in_extern_c = std::find(lines.begin(), lines.end(), tag.line) != lines.end();
        if (in_extern_c && tag.modifiers.find(EXTERN_C) == std::string::npos) {
            tag.modifiers += " ";
            tag.modifiers += EXTERN_C;
        }
    }
}

static int64_t first_function_line(const std::vector<found_tag>& tags, const std::string& main_file) {
    for (const found_tag& tag : tags) {
        if (tag.kind == tag_kind::function && !tag.scoped && tag.file == main_file) {
            return tag.line;
        }
    }

    return -1;
}

// The first tag whose line takes a function's address, `&name` or `(name)`, as a callback argument does
static int64_t first_function_pointer_line(const std::vector<found_tag>& tags) {
    for (const found_tag& tag : tags) {
        for (const found_tag& function : tags) {
            if (function.kind != tag_kind::function || function.skipped || tag.line == function.line) {
                continue;
            }

            if (tag.code.find("&" + function.name) != std::string::npos ||
                tag.code.find("(" + function.name + ")") != std::string::npos) {
                return tag.line;
            }
        }
    }

    return -1;
}

std::string sketch_lines(std::string_view preprocessed, const std::vector<std::string>& ino_files,
                         bool& line_too_long) {
    std::string kept;
    bool in_sketch = false;
    size_t start = 0;
    line_too_long = false;
    while (start < preprocessed.size()) {
        size_t end = preprocessed.find('\n', start);
        if (end == std::string_view::npos) {
            end = preprocessed.size();
        }

        if (end - start >= SCANNED_LINE_MAX) {
            line_too_long = true;
            break;
        }

        std::string_view line = preprocessed.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        std::string file;
        if (read_arduino_line_marker(line, file)) {
            in_sketch = std::find(ino_files.begin(), ino_files.end(), file) != ino_files.end();
        }

        if (in_sketch) {
            kept += line;
            kept += '\n';
        }
    }

    return kept;
}

prototype_plan find_prototypes(std::string_view lines, const std::string& main_file,
                               const std::vector<ino_text>& files) {
    std::vector<scanned_line> scanned;
    std::vector<found_tag> tags = scan_tags(lines, scanned);
    file_texts texts = texts_by_file(files);

    for (found_tag& tag : tags) {
        tag.skipped = tag.kind == tag_kind::other || tag.scoped;
    }

    add_prototypes(tags, texts);
    remove_defined_prototypes(tags);
    skip_duplicates(tags);
    for (found_tag& tag : tags) {
        if (!tag.skipped) {
            tag.skipped = prototype_and_code_differ(tag, texts);
        }
    }

    add_c_linkage(tags, texts);

    prototype_plan plan;
    for (const found_tag& tag : tags) {
        bool blank = tag.prototype.find_first_not_of(" \t\n\v\f\r") == std::string::npos;
        if (!tag.skipped && !blank) {
            plan.prototypes.push_back({tag.prototype, tag.modifiers, tag.file, static_cast<uint32_t>(tag.line)});
        }
    }

    int64_t function_line = first_function_line(tags, main_file);
    int64_t pointer_line = first_function_pointer_line(tags);
    int64_t insert_line = 0;
    if (function_line != -1 && pointer_line != -1) {
        insert_line = std::min(function_line, pointer_line);
    } else if (function_line != -1) {
        insert_line = function_line;
    } else if (pointer_line != -1) {
        insert_line = pointer_line;
    }

    plan.insert_line = static_cast<uint32_t>(insert_line);
    return plan;
}

bool insert_prototypes(const merged_sketch& merged, const prototype_plan& plan, std::string& source) {
    source.clear();
    for (size_t i = 0; i < merged.text.size(); i++) {
        if (merged.text[i] == '\r') {
            source += '\n';
            if (i + 1 < merged.text.size() && merged.text[i + 1] == '\n') {
                i++;
            }
        } else {
            source += merged.text[i];
        }
    }

    const size_t rows = static_cast<size_t>(std::count(source.begin(), source.end(), '\n')) + 1;
    if (plan.insert_line > rows - 1) {
        return false;
    }

    if (plan.prototypes.empty()) {
        return true;
    }

    const size_t insertion_row = plan.insert_line + merged.line_offset - 1;
    size_t offset = 0;
    for (size_t row = 0; row < insertion_row && offset < source.size(); row++) {
        offset = source.find('\n', offset);
        offset = offset == std::string::npos ? source.size() : offset + 1;
    }

    // arduino-cli places the prototypes one character past the start of the text when they go before its first row
    if (insertion_row == 0) {
        offset = std::min<size_t>(1, source.size());
    }

    std::string section;
    for (const function_prototype& prototype : plan.prototypes) {
        if (prototype.declaration.find('=') != std::string::npos) {
            continue;
        }

        if (!section.empty()) {
            section += '\n';
        }

        section += line_mark(prototype.line, prototype.file) + "\n";
        if (!prototype.modifiers.empty()) {
            section += prototype.modifiers + " ";
        }

        section += prototype.declaration;
    }

    // arduino-cli names the first prototype's file here, even when the main file's lines follow
    section += "\n" + line_mark(plan.insert_line, plan.prototypes.front().file) + "\n";
    source = source.substr(0, offset) + section + source.substr(offset);

    return true;
}
