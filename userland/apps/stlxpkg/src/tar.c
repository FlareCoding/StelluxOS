#include "tar.h"

#include <string.h>

#define NAME_OFFSET     0
#define NAME_SIZE       100
#define MODE_OFFSET     100
#define MODE_SIZE       8
#define SIZE_OFFSET     124
#define SIZE_SIZE       12
#define CHECKSUM_OFFSET 148
#define CHECKSUM_SIZE   8
#define TYPE_OFFSET     156
#define TARGET_OFFSET   157
#define TARGET_SIZE     100
#define MAGIC_OFFSET    257
#define PREFIX_OFFSET   345
#define PREFIX_SIZE     155
#define USTAR_MAGIC     "ustar"
#define MODE_MASK       0777

void tar_init(tar_reader* reader, const tar_handlers* handlers) {
    memset(reader, 0, sizeof(*reader));
    reader->handlers = *handlers;
    reader->state = TAR_READING_HEADER;
}

bool tar_ended(const tar_reader* reader) {
    return reader->state == TAR_ENDED;
}

/* An octal field, which may start with spaces and ends at a NUL, a space or the field's end */
static bool parse_octal(const uint8_t* field, size_t size, uint64_t* value) {
    size_t i = 0;
    while (i < size && field[i] == ' ') {
        i++;
    }

    size_t first_digit = i;
    *value = 0;
    while (i < size && field[i] >= '0' && field[i] <= '7') {
        *value = *value * 8 + (uint64_t)(field[i] - '0');
        i++;
    }

    return i > first_digit && (i == size || field[i] == '\0' || field[i] == ' ');
}

static bool checksum_matches(const uint8_t* block) {
    uint64_t stated = 0;
    if (!parse_octal(block + CHECKSUM_OFFSET, CHECKSUM_SIZE, &stated)) {
        return false;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        bool in_checksum = i >= CHECKSUM_OFFSET && i < CHECKSUM_OFFSET + CHECKSUM_SIZE;
        sum += in_checksum ? ' ' : block[i];
    }

    return sum == stated;
}

static bool is_zero_block(const uint8_t* block) {
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (block[i] != 0) {
            return false;
        }
    }

    return true;
}

/* A header field is NUL terminated only when it is shorter than the field */
static size_t field_length(const uint8_t* field, size_t size) {
    size_t length = 0;
    while (length < size && field[length] != '\0') {
        length++;
    }

    return length;
}

static bool copy_value(char* destination, const char* value, size_t length) {
    if (length >= TAR_PATH_MAX) {
        return false;
    }

    memcpy(destination, value, length);
    destination[length] = '\0';

    return true;
}

static bool parse_decimal(const char* text, size_t length, uint64_t* value) {
    *value = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9' || i == 19) {
            return false;
        }

        *value = *value * 10 + (uint64_t)(text[i] - '0');
    }

    return length > 0;
}

/* Applies the pax records "<length> <key>=<value>\n" that precede an entry, the path, target and size ones */
static int apply_pax_records(tar_reader* reader) {
    size_t offset = 0;
    while (offset < reader->pax_length) {
        const char* record = reader->pax + offset;
        size_t available = reader->pax_length - offset;
        size_t digits = 0;
        while (digits < available && record[digits] >= '0' && record[digits] <= '9') {
            digits++;
        }

        uint64_t length = 0;
        if (digits >= available || record[digits] != ' ' || !parse_decimal(record, digits, &length) ||
            length > available || length <= digits + 1 || record[length - 1] != '\n') {
            return TAR_ERR_HEADER;
        }

        const char* key = record + digits + 1;
        const char* end = record + length - 1;
        const char* equals = memchr(key, '=', (size_t)(end - key));
        if (!equals) {
            return TAR_ERR_HEADER;
        }

        size_t key_length = (size_t)(equals - key);
        size_t value_length = (size_t)(end - equals - 1);
        bool applied = true;
        if (key_length == 4 && memcmp(key, "path", 4) == 0) {
            applied = copy_value(reader->entry.path, equals + 1, value_length);
        } else if (key_length == 8 && memcmp(key, "linkpath", 8) == 0) {
            applied = copy_value(reader->entry.target, equals + 1, value_length);
        } else if (key_length == 4 && memcmp(key, "size", 4) == 0) {
            applied = parse_decimal(equals + 1, value_length, &reader->entry.size);
        }

        if (!applied) {
            return TAR_ERR_HEADER;
        }

        offset += (size_t)length;
    }

    return TAR_OK;
}

/* Turns "./usr/bin/" into "usr/bin" and the archive's root into "". A path that could leave the root has an empty,
 * `.` or `..` component, an absolute one an empty first component. */
static int clean_path(char* path) {
    size_t length = strlen(path);
    while (length > 0 && path[length - 1] == '/') {
        path[--length] = '\0';
    }

    const char* start = path;
    if (strncmp(path, "./", 2) == 0) {
        start += 2;
    } else if (strcmp(path, ".") == 0) {
        start += 1;
    }

    memmove(path, start, strlen(start) + 1);

    const char* component = path;
    while (path[0] != '\0') {
        size_t component_length = strcspn(component, "/");
        bool is_dot = component_length == 1 && component[0] == '.';
        bool is_dot_dot = component_length == 2 && component[0] == '.' && component[1] == '.';
        if (component_length == 0 || is_dot || is_dot_dot) {
            return TAR_ERR_PATH;
        }

        if (component[component_length] == '\0') {
            break;
        }

        component += component_length + 1;
    }

    return TAR_OK;
}

/* Skips past the padding that fills the last data block, ending a file for its handler first */
static int finish_data(tar_reader* reader) {
    if (reader->state == TAR_READING_FILE && reader->handlers.end_file(reader->handlers.context) != 0) {
        return TAR_ERR_HANDLER;
    }

    reader->state = reader->padding > 0 ? TAR_SKIPPING : TAR_READING_HEADER;
    reader->remaining = reader->padding;
    reader->padding = 0;

    return TAR_OK;
}

static int start_data(tar_reader* reader, tar_state state, uint64_t size) {
    reader->state = state;
    reader->remaining = size;
    reader->padding = (TAR_BLOCK_SIZE - size % TAR_BLOCK_SIZE) % TAR_BLOCK_SIZE;

    return size == 0 ? finish_data(reader) : TAR_OK;
}

/* Fills the entry from its ustar header, joining the path from the prefix and name fields */
static void fill_entry(tar_entry* entry, const uint8_t* block, uint64_t mode, uint64_t size) {
    size_t prefix_length = field_length(block + PREFIX_OFFSET, PREFIX_SIZE);
    size_t name_start = 0;
    if (prefix_length > 0) {
        memcpy(entry->path, block + PREFIX_OFFSET, prefix_length);
        entry->path[prefix_length] = '/';
        name_start = prefix_length + 1;
    }

    size_t name_length = field_length(block + NAME_OFFSET, NAME_SIZE);
    memcpy(entry->path + name_start, block + NAME_OFFSET, name_length);
    entry->path[name_start + name_length] = '\0';

    size_t target_length = field_length(block + TARGET_OFFSET, TARGET_SIZE);
    memcpy(entry->target, block + TARGET_OFFSET, target_length);
    entry->target[target_length] = '\0';
    entry->mode = (uint32_t)(mode & MODE_MASK);
    entry->size = size;
}

static int set_entry_type(tar_entry* entry, char type_flag) {
    if (type_flag == '0' || type_flag == '\0') {
        entry->type = TAR_FILE;
    } else if (type_flag == '5') {
        entry->type = TAR_DIRECTORY;
    } else if (type_flag == '2') {
        entry->type = TAR_SYMLINK;
    } else {
        return TAR_ERR_TYPE;
    }

    return TAR_OK;
}

static int read_entry(tar_reader* reader, char type_flag, uint64_t mode, uint64_t size) {
    tar_entry* entry = &reader->entry;
    fill_entry(entry, reader->block, mode, size);
    int rc = apply_pax_records(reader);
    reader->pax_length = 0;
    if (rc == TAR_OK) {
        rc = clean_path(entry->path);
    }

    if (rc == TAR_OK) {
        rc = set_entry_type(entry, type_flag);
    }

    if (rc != TAR_OK) {
        return rc;
    }

    bool is_root = entry->path[0] == '\0';
    if (is_root && entry->type != TAR_DIRECTORY) {
        return TAR_ERR_PATH;
    }

    if (!is_root && reader->handlers.entry(reader->handlers.context, entry) != 0) {
        return TAR_ERR_HANDLER;
    }

    return start_data(reader, entry->type == TAR_FILE ? TAR_READING_FILE : TAR_SKIPPING, entry->size);
}

static int read_header(tar_reader* reader) {
    const uint8_t* block = reader->block;
    if (is_zero_block(block)) {
        reader->state = TAR_ENDED;
        return TAR_OK;
    }

    uint64_t mode = 0;
    uint64_t size = 0;
    bool is_ustar = memcmp(block + MAGIC_OFFSET, USTAR_MAGIC, strlen(USTAR_MAGIC)) == 0;
    if (!is_ustar || !checksum_matches(block) || !parse_octal(block + MODE_OFFSET, MODE_SIZE, &mode) ||
        !parse_octal(block + SIZE_OFFSET, SIZE_SIZE, &size)) {
        return TAR_ERR_HEADER;
    }

    char type_flag = (char)block[TYPE_OFFSET];
    if (type_flag == 'x' && size > TAR_PAX_MAX) {
        return TAR_ERR_HEADER;
    }

    if (type_flag == 'x') {
        reader->pax_length = 0;
        return start_data(reader, TAR_READING_PAX, size);
    }

    if (type_flag == 'g') {
        return start_data(reader, TAR_SKIPPING, size);
    }

    return read_entry(reader, type_flag, mode, size);
}

static int take_data(tar_reader* reader, const uint8_t* bytes, size_t length) {
    if (reader->state == TAR_READING_FILE && reader->handlers.data(reader->handlers.context, bytes, length) != 0) {
        return TAR_ERR_HANDLER;
    }

    if (reader->state == TAR_READING_PAX) {
        memcpy(reader->pax + reader->pax_length, bytes, length);
        reader->pax_length += length;
    }

    reader->remaining -= length;

    return reader->remaining == 0 ? finish_data(reader) : TAR_OK;
}

int tar_feed(tar_reader* reader, const uint8_t* bytes, size_t length) {
    while (length > 0 && reader->state != TAR_ENDED) {
        size_t taken = 0;
        int rc = TAR_OK;
        if (reader->state == TAR_READING_HEADER) {
            taken = length < TAR_BLOCK_SIZE - reader->block_length ? length : TAR_BLOCK_SIZE - reader->block_length;
            memcpy(reader->block + reader->block_length, bytes, taken);
            reader->block_length += taken;
        } else {
            taken = length < reader->remaining ? length : (size_t)reader->remaining;
            rc = take_data(reader, bytes, taken);
        }

        if (rc == TAR_OK && reader->block_length == TAR_BLOCK_SIZE) {
            reader->block_length = 0;
            rc = read_header(reader);
        }

        if (rc != TAR_OK) {
            return rc;
        }

        bytes += taken;
        length -= taken;
    }

    return TAR_OK;
}
