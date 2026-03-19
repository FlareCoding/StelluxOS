#include "drivers/usb/hid/hid_parser.h"
#include "mm/heap.h"

namespace usb::hid {

constexpr uint8_t LONG_ITEM_PREFIX = 0xFE;
constexpr uint8_t MAX_EXPLICIT_USAGES = 32;
constexpr uint8_t GLOBAL_STACK_DEPTH = 4;

// Advance past one item in the descriptor, returning the data size consumed.
// Returns the number of bytes the full item occupies (prefix + data),
// or 0 if the stream is malformed.
static size_t item_length(const uint8_t* descriptor, size_t offset, size_t length) {
    if (offset >= length) {
        return 0;
    }

    uint8_t prefix = descriptor[offset];

    // Long item: prefix(1) + data_size(1) + tag(1) + data(data_size)
    if (prefix == LONG_ITEM_PREFIX) {
        if (offset + 1 >= length) {
            return 0;
        }
        uint8_t data_size = descriptor[offset + 1];
        size_t total = 3 + static_cast<size_t>(data_size);
        if (offset + total > length) {
            return 0;
        }
        return total;
    }

    // Short item: prefix(1) + data(size)
    uint8_t size = prefix & 0x03;
    if (size == 3) size = 4; // size encoding: 0,1,2,3 → 0,1,2,4 bytes
    if (offset + 1 + size > length) {
        return 0;
    }
    return 1 + size;
}

// Decode a short item at the given offset
static report_item decode_short_item(const uint8_t* descriptor, size_t offset) {
    uint8_t prefix = descriptor[offset];
    uint8_t size = prefix & 0x03;
    if (size == 3) size = 4;

    uint8_t type = (prefix >> 2) & 0x03;
    uint8_t tag = (prefix >> 4) & 0x0F;

    uint32_t data = 0;
    for (uint8_t i = 0; i < size; i++) {
        data |= static_cast<uint32_t>(descriptor[offset + 1 + i]) << (8 * i);
    }

    return { static_cast<item_type>(type), tag, size, data };
}

// Global state tracked during layout building
struct global_state {
    uint16_t usage_page = 0;
    uint8_t  report_size = 0;
    uint8_t  report_count = 0;
    int32_t  logical_minimum = 0;
    int32_t  logical_maximum = 0;
};

// Local state reset after each Main item
struct local_state {
    uint16_t usages[MAX_EXPLICIT_USAGES] = {};
    uint8_t  usage_count = 0;
    uint16_t usage_minimum = 0;
    uint16_t usage_maximum = 0;
    bool     has_usage_range = false;

    void reset() {
        usage_count = 0;
        usage_minimum = 0;
        usage_maximum = 0;
        has_usage_range = false;
    }
};

// Count total items in the descriptor (pass 1 for items)
static size_t count_items(const uint8_t* descriptor, size_t length) {
    size_t count = 0;
    size_t offset = 0;
    while (offset < length) {
        size_t len = item_length(descriptor, offset, length);
        if (len == 0) {
            break;
        }
        // Only count short items (skip long items)
        if (descriptor[offset] != LONG_ITEM_PREFIX) {
            count++;
        }
        offset += len;
    }
    return count;
}

// Parse items (pass 2 for items)
static size_t parse_items(const uint8_t* descriptor, size_t length,
                          report_item* items, size_t max_items) {
    size_t count = 0;
    size_t offset = 0;
    while (offset < length && count < max_items) {
        size_t len = item_length(descriptor, offset, length);
        if (len == 0) {
            break;
        }
        if (descriptor[offset] != LONG_ITEM_PREFIX) {
            items[count++] = decode_short_item(descriptor, offset);
        }
        offset += len;
    }
    return count;
}

// Count how many field_info entries the items will produce (pass 1 for fields)
static uint16_t count_fields(const report_item* items, size_t num_items) {
    uint16_t field_count = 0;
    uint8_t report_count = 0;

    for (size_t i = 0; i < num_items; i++) {
        const auto& item = items[i];

        if (item.type == item_type::global &&
            static_cast<global_item_tag>(item.tag) == global_item_tag::report_count) {
            report_count = static_cast<uint8_t>(item.data);
        }

        if (item.type == item_type::main &&
            static_cast<main_item_tag>(item.tag) == main_item_tag::input) {
            bool is_constant = item.data & 0x01;
            if (!is_constant) {
                field_count += report_count;
            }
        }
    }

    return field_count;
}

// Build the field layout from parsed items (pass 2 for fields)
static uint16_t build_fields(const report_item* items, size_t num_items,
                             field_info* fields, uint16_t max_fields) {
    global_state globals = {};
    global_state global_stack[GLOBAL_STACK_DEPTH] = {};
    uint8_t stack_depth = 0;
    local_state locals = {};
    uint16_t current_bit_offset = 0;
    uint16_t field_idx = 0;

    for (size_t i = 0; i < num_items; i++) {
        const auto& item = items[i];

        switch (item.type) {
        case item_type::global:
            switch (static_cast<global_item_tag>(item.tag)) {
            case global_item_tag::usage_page:
                globals.usage_page = static_cast<uint16_t>(item.data);
                break;
            case global_item_tag::report_size:
                globals.report_size = static_cast<uint8_t>(item.data);
                break;
            case global_item_tag::report_count:
                globals.report_count = static_cast<uint8_t>(item.data);
                break;
            case global_item_tag::logical_minimum:
                globals.logical_minimum = static_cast<int32_t>(item.data);
                break;
            case global_item_tag::logical_maximum:
                globals.logical_maximum = static_cast<int32_t>(item.data);
                break;
            case global_item_tag::push:
                if (stack_depth < GLOBAL_STACK_DEPTH) {
                    global_stack[stack_depth++] = globals;
                }
                break;
            case global_item_tag::pop:
                if (stack_depth > 0) {
                    globals = global_stack[--stack_depth];
                }
                break;
            default:
                break;
            }
            break;

        case item_type::local:
            switch (static_cast<local_item_tag>(item.tag)) {
            case local_item_tag::usage:
                if (locals.usage_count < MAX_EXPLICIT_USAGES) {
                    locals.usages[locals.usage_count++] = static_cast<uint16_t>(item.data);
                }
                break;
            case local_item_tag::usage_minimum:
                locals.usage_minimum = static_cast<uint16_t>(item.data);
                locals.has_usage_range = true;
                break;
            case local_item_tag::usage_maximum:
                locals.usage_maximum = static_cast<uint16_t>(item.data);
                locals.has_usage_range = true;
                break;
            default:
                break;
            }
            break;

        case item_type::main:
            if (static_cast<main_item_tag>(item.tag) == main_item_tag::input) {
                bool is_constant = item.data & 0x01;

                if (is_constant) {
                    // Constant fields still consume bits in the report
                    current_bit_offset += globals.report_size * globals.report_count;
                } else {
                    for (uint8_t j = 0; j < globals.report_count && field_idx < max_fields; j++) {
                        uint16_t usage = 0;

                        // Resolve usage for this field
                        if (j < locals.usage_count) {
                            usage = locals.usages[j];
                        } else if (locals.has_usage_range) {
                            uint16_t range_idx = j - locals.usage_count;
                            uint16_t candidate = locals.usage_minimum + range_idx;
                            usage = (candidate <= locals.usage_maximum)
                                  ? candidate : locals.usage_maximum;
                        } else if (locals.usage_count > 0) {
                            usage = locals.usages[locals.usage_count - 1];
                        }

                        fields[field_idx++] = {
                            current_bit_offset,
                            globals.report_size,
                            globals.usage_page,
                            usage
                        };
                        current_bit_offset += globals.report_size;
                    }
                }
                locals.reset();
            } else if (static_cast<main_item_tag>(item.tag) == main_item_tag::collection ||
                       static_cast<main_item_tag>(item.tag) == main_item_tag::end_collection) {
                locals.reset();
            }
            break;

        default:
            break;
        }
    }

    return field_idx;
}

void report_layout::destroy() {
    if (fields) {
        heap::ufree(fields);
        fields = nullptr;
    }
    num_fields = 0;
}

int32_t parse_report_descriptor(const uint8_t* descriptor, size_t length,
                                report_layout& out) {
    out.fields = nullptr;
    out.num_fields = 0;

    if (!descriptor || length == 0) {
        return -1;
    }

    // Pass 1: count items
    size_t item_count = count_items(descriptor, length);
    if (item_count == 0) {
        return -1;
    }

    // Pass 2: parse items into temp array
    auto* items = static_cast<report_item*>(
        heap::ualloc(item_count * sizeof(report_item)));
    if (!items) {
        return -1;
    }

    size_t parsed = parse_items(descriptor, length, items, item_count);

    // Pass 1 (fields): count fields from parsed items
    uint16_t field_count = count_fields(items, parsed);
    if (field_count == 0) {
        heap::ufree(items);
        return -1;
    }

    // Pass 2 (fields): allocate and build
    out.fields = static_cast<field_info*>(
        heap::ualloc(field_count * sizeof(field_info)));
    if (!out.fields) {
        heap::ufree(items);
        return -1;
    }

    out.num_fields = build_fields(items, parsed, out.fields, field_count);

    heap::ufree(items);
    return 0;
}

} // namespace usb::hid
