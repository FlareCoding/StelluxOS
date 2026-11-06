#include "layout_store.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

/* A line of the text being parsed, without its newline */
struct text_line {
    const char* start = nullptr;
    const char* end = nullptr;
};

static constexpr size_t FILE_MAX = 32 * 1024;
static constexpr const char FILE_COMMENT[] = "# stlxdm's output layouts, newest first\n";

static char g_file_text[FILE_MAX];

static bool starts_with(const text_line& l, const char* prefix) {
    size_t length = strlen(prefix);
    return static_cast<size_t>(l.end - l.start) >= length && memcmp(l.start, prefix, length) == 0;
}

static bool is_blank(const text_line& l) {
    for (const char* p = l.start; p < l.end; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r') {
            return false;
        }
    }

    return true;
}

/* Reads the decimal at `cursor`, ending at a space or the line's end, into `out`, and returns whether it lies from
 * `low` to `high` */
static bool read_number(const char*& cursor, const char* end, int64_t low, int64_t high, int64_t& out) {
    bool negative = cursor < end && *cursor == '-';
    const char* digits = negative ? cursor + 1 : cursor;
    const char* p = digits;
    int64_t value = 0;
    while (p < end && *p >= '0' && *p <= '9' && value <= (int64_t{ 1 } << 33)) {
        value = value * 10 + (*p - '0');
        p++;
    }

    if (p == digits || (p < end && *p != ' ')) {
        return false;
    }

    out = negative ? -value : value;
    cursor = p;

    return out >= low && out <= high;
}

static bool skip_space(const char*& cursor, const char* end) {
    if (cursor >= end || *cursor != ' ') {
        return false;
    }

    cursor++;

    return true;
}

static bool read_maker(const char*& cursor, const char* end, char* maker) {
    if (cursor < end && *cursor == '-' && (cursor + 1 == end || cursor[1] == ' ')) {
        cursor++;
        return true;
    }

    for (int i = 0; i < 3; i++) {
        char c = cursor + i < end ? cursor[i] : '\0';
        if (!((c >= 'A' && c <= 'Z') || c == '?')) {
            return false;
        }

        maker[i] = c;
    }

    cursor += 3;

    return true;
}

/* Reads a `monitor = ` line's eight fields and its name into `out` */
static bool read_monitor(const text_line& l, layout_store::line& out) {
    const char* cursor = l.start + strlen("monitor = ");
    int64_t output = 0;
    int64_t product = 0;
    int64_t serial = 0;
    int64_t place[4] = {};

    bool fields_read = read_number(cursor, l.end, 0, layout::MEMBERS_MAX - 1, output) && skip_space(cursor, l.end) &&
                       read_maker(cursor, l.end, out.monitor.maker) && skip_space(cursor, l.end) &&
                       read_number(cursor, l.end, 0, UINT16_MAX, product) && skip_space(cursor, l.end) &&
                       read_number(cursor, l.end, 0, UINT32_MAX, serial);
    for (int i = 0; i < 4 && fields_read; i++) {
        int64_t low = i < 2 ? -layout::DESKTOP_LIMIT : 1;
        fields_read = skip_space(cursor, l.end) && read_number(cursor, l.end, low, layout::DESKTOP_LIMIT, place[i]);
    }

    if (!fields_read) {
        return false;
    }

    size_t name_length = cursor < l.end ? static_cast<size_t>(l.end - cursor - 1) : 0;
    if (name_length >= sizeof(out.monitor.name)) {
        return false;
    }

    if (cursor < l.end) {
        memcpy(out.monitor.name, cursor + 1, name_length);
    }

    out.output = static_cast<uint32_t>(output);
    out.monitor.product = static_cast<uint32_t>(product);
    out.monitor.serial = static_cast<uint32_t>(serial);

    out.x = static_cast<int32_t>(place[0]);
    out.y = static_cast<int32_t>(place[1]);
    out.width = static_cast<int32_t>(place[2]);
    out.height = static_cast<int32_t>(place[3]);

    int64_t right = place[0] + place[2];
    int64_t bottom = place[1] + place[3];

    return right <= layout::DESKTOP_LIMIT && bottom <= layout::DESKTOP_LIMIT;
}

/* Whether `s`, read with `mains` main lines, has exactly one, naming one of its lines */
static bool is_whole(const layout_store::section& s, uint32_t mains) {
    bool main_named = false;
    for (uint32_t i = 0; i < s.count; i++) {
        main_named = main_named || s.lines[i].output == s.main;
    }

    return mains == 1 && main_named;
}

/* Appends to `buffer` at `length` what `format` prints, counting it in `length` even past `size` */
__attribute__((format(printf, 4, 5)))
static void append(char* buffer, size_t size, size_t& length, const char* format, ...) {
    va_list args;
    va_start(args, format);
    bool has_room = length < size;
    int written = vsnprintf(has_room ? buffer + length : nullptr, has_room ? size - length : 0, format, args);
    va_end(args);
    length += written > 0 ? static_cast<size_t>(written) : 0;
}

bool layout_store::parse(const char* text, size_t length) {
    m_count = 0;

    section current;
    uint32_t mains = 0;
    uint32_t number = 0;
    bool is_malformed = false;
    auto finish = [&]() {
        if (is_malformed || !is_whole(current, mains)) {
            printf("stlxdm: layouts.conf's section %u is malformed, skipping it\r\n", number);
        } else if (m_count < LAYOUTS_KEPT) {
            m_sections[m_count++] = current;
        }
    };

    const char* end = text + length;
    for (const char* start = text; start < end;) {
        const char* newline = static_cast<const char*>(memchr(start, '\n', static_cast<size_t>(end - start)));
        text_line l = { start, newline ? newline : end };
        start = newline ? newline + 1 : end;
        if (starts_with(l, "#") || is_blank(l)) {
            continue;
        }

        if (l.end - l.start == 8 && starts_with(l, "[layout]")) {
            if (number != 0) {
                finish();
            }

            current = {};
            mains = 0;
            number++;
            is_malformed = false;
            continue;
        }

        if (number == 0) {
            printf("stlxdm: layouts.conf has a line before its first section, ignoring the file\r\n");
            return false;
        }

        if (starts_with(l, "main = ")) {
            int64_t main = 0;
            const char* cursor = l.start + strlen("main = ");
            bool is_read = read_number(cursor, l.end, 0, layout::MEMBERS_MAX - 1, main) && cursor == l.end;
            is_malformed = is_malformed || !is_read;

            current.main = static_cast<uint32_t>(main);
            mains++;
            continue;
        }

        line read = {};
        bool is_monitor = starts_with(l, "monitor = ") && current.count < layout::MEMBERS_MAX && read_monitor(l, read);
        for (uint32_t i = 0; i < current.count && is_monitor; i++) {
            is_monitor = current.lines[i].output != read.output;
        }

        if (!is_monitor) {
            is_malformed = true;
            continue;
        }

        current.lines[current.count++] = read;
    }

    if (number != 0) {
        finish();
    }

    return true;
}

void layout_store::read(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) {
            printf("stlxdm: could not read %s: %s\r\n", path, strerror(errno));
        }

        return;
    }

    size_t length = fread(g_file_text, 1, sizeof(g_file_text), f);
    bool is_whole_file = length < sizeof(g_file_text) && !ferror(f);
    fclose(f);
    if (!is_whole_file) {
        printf("stlxdm: %s is too large or unreadable, ignoring it\r\n", path);
        return;
    }

    parse(g_file_text, length);
}

bool layout_store::pair(const section& s, const member* members, uint32_t count, uint32_t* line_of) const {
    if (s.count != count) {
        return false;
    }

    bool used[layout::MEMBERS_MAX] = {};
    for (uint32_t i = 0; i < count; i++) {
        bool is_alike = false;
        for (uint32_t j = 0; j < count; j++) {
            is_alike = is_alike || (j != i && members[i].monitor == members[j].monitor);
        }

        int32_t found = -1;
        for (uint32_t k = 0; k < s.count && found < 0; k++) {
            bool fits = !used[k] && s.lines[k].monitor == members[i].monitor;
            if (fits && (!is_alike || s.lines[k].output == members[i].output)) {
                found = static_cast<int32_t>(k);
            }
        }

        if (found < 0) {
            return false;
        }

        used[found] = true;
        line_of[i] = static_cast<uint32_t>(found);
    }

    return true;
}

bool layout_store::match(const member* members, uint32_t count, layout::arrangement& out) const {
    uint32_t line_of[layout::MEMBERS_MAX];
    for (uint32_t i = 0; i < m_count; i++) {
        const section& s = m_sections[i];
        if (!pair(s, members, count, line_of)) {
            continue;
        }

        out.count = count;
        for (uint32_t j = 0; j < count; j++) {
            const line& l = s.lines[line_of[j]];
            out.entries[j] = { members[j].output, l.x, l.y, l.width, l.height };
            if (l.output == s.main) {
                out.main = members[j].output;
            }
        }

        return true;
    }

    return false;
}

void layout_store::remember(const member* members, uint32_t count, const layout& places) {
    section newest;
    newest.main = places.main_output();
    newest.count = count;
    for (uint32_t i = 0; i < count; i++) {
        const damage_list::rect& r = places.at(i).rect;
        newest.lines[i] = { members[i].output, members[i].monitor, r.x, r.y, r.w, r.h };
    }

    section kept[LAYOUTS_KEPT];
    uint32_t kept_count = 0;
    kept[kept_count++] = newest;

    uint32_t line_of[layout::MEMBERS_MAX];
    for (uint32_t i = 0; i < m_count && kept_count < LAYOUTS_KEPT; i++) {
        if (!pair(m_sections[i], members, count, line_of)) {
            kept[kept_count++] = m_sections[i];
        }
    }

    memcpy(m_sections, kept, sizeof(section) * kept_count);
    m_count = kept_count;
}

size_t layout_store::format(char* buffer, size_t size) const {
    size_t length = 0;
    append(buffer, size, length, "%s", FILE_COMMENT);
    for (uint32_t i = 0; i < m_count; i++) {
        const section& s = m_sections[i];
        append(buffer, size, length, "%s[layout]\nmain = %u\n", i == 0 ? "" : "\n", s.main);
        for (uint32_t j = 0; j < s.count; j++) {
            const line& l = s.lines[j];
            const char* maker = l.monitor.maker[0] ? l.monitor.maker : "-";
            const char* space = l.monitor.name[0] ? " " : "";
            append(buffer, size, length, "monitor = %u %s %u %u %d %d %d %d%s%s\n", l.output, maker, l.monitor.product,
                   l.monitor.serial, l.x, l.y, l.width, l.height, space, l.monitor.name);
        }
    }

    return length;
}

int layout_store::write(const char* path) const {
    size_t length = format(g_file_text, sizeof(g_file_text));
    if (length >= sizeof(g_file_text)) {
        return ENOSPC;
    }

    char dir[256];
    char temporary[256];
    const char* slash = strrchr(path, '/');
    size_t dir_length = slash ? static_cast<size_t>(slash - path) : 0;
    int temporary_length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (dir_length >= sizeof(dir) || temporary_length >= static_cast<int>(sizeof(temporary))) {
        return ENAMETOOLONG;
    }

    memcpy(dir, path, dir_length);
    dir[dir_length] = '\0';
    if (dir_length != 0 && mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return errno;
    }

    FILE* f = fopen(temporary, "w");
    if (!f) {
        return errno;
    }

    errno = 0;
    bool written = fwrite(g_file_text, 1, length, f) == length;
    written = fclose(f) == 0 && written;
    if (!written || rename(temporary, path) != 0) {
        int error = errno != 0 ? errno : EIO;
        remove(temporary);
        return error;
    }

    return 0;
}
