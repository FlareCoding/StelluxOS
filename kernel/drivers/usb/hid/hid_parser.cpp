#include "drivers/usb/hid/hid_parser.h"
#include "mm/heap.h"

namespace usb::hid {

constexpr uint8_t LONG_ITEM_PREFIX = 0xFE;
constexpr uint8_t MAX_EXPLICIT_USAGES = 32;
constexpr uint8_t GLOBAL_STACK_DEPTH = 4;
constexpr uint16_t INVALID_REPORT_INDEX = 0xFFFFu;

static size_t item_length(const uint8_t* descriptor, size_t offset, size_t length) {
    if (offset >= length) {
        return 0;
    }

    uint8_t prefix = descriptor[offset];
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

    uint8_t size = prefix & 0x03u;
    if (size == 3) {
        size = 4;
    }
    if (offset + 1 + size > length) {
        return 0;
    }
    return 1 + size;
}

static report_item decode_short_item(const uint8_t* descriptor, size_t offset) {
    uint8_t prefix = descriptor[offset];
    uint8_t size = prefix & 0x03u;
    if (size == 3) {
        size = 4;
    }

    uint32_t data = 0;
    for (uint8_t i = 0; i < size; i++) {
        data |= static_cast<uint32_t>(descriptor[offset + 1 + i]) << (8u * i);
    }

    return {
        static_cast<item_type>((prefix >> 2) & 0x03u),
        static_cast<uint8_t>((prefix >> 4) & 0x0Fu),
        size,
        data,
    };
}

static int32_t sign_extend(uint32_t value, uint8_t payload_bytes) {
    if (payload_bytes == 0) {
        return 0;
    }

    uint8_t bits = static_cast<uint8_t>(payload_bytes * 8u);
    if (bits >= 32) {
        return static_cast<int32_t>(value);
    }

    uint32_t mask = (1u << bits) - 1u;
    value &= mask;
    uint32_t sign_bit = 1u << (bits - 1u);
    if ((value & sign_bit) != 0) {
        value |= ~mask;
    }
    return static_cast<int32_t>(value);
}

struct global_state {
    uint16_t usage_page = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint8_t  report_id = 0;
    int32_t  logical_minimum = 0;
    int32_t  logical_maximum = 0;
};

struct local_state {
    uint32_t usages[MAX_EXPLICIT_USAGES] = {};
    uint8_t  usage_count = 0;
    uint32_t usage_minimum = 0;
    uint32_t usage_maximum = 0;
    bool     has_usage_range = false;

    void reset() {
        usage_count = 0;
        usage_minimum = 0;
        usage_maximum = 0;
        has_usage_range = false;
    }
};

struct report_accumulator {
    bool     seen = false;
    uint8_t  report_id = 0;
    uint32_t field_count = 0;
    uint32_t body_bits = 0;
};

static bool count_items(const uint8_t* descriptor, size_t length, size_t& out_count) {
    out_count = 0;
    size_t offset = 0;
    while (offset < length) {
        size_t len = item_length(descriptor, offset, length);
        if (len == 0) {
            return false;
        }

        if (descriptor[offset] != LONG_ITEM_PREFIX) {
            out_count++;
        }
        offset += len;
    }
    return true;
}

static bool parse_items(const uint8_t* descriptor, size_t length,
                        report_item* items, size_t max_items, size_t& out_count) {
    out_count = 0;
    size_t offset = 0;
    while (offset < length) {
        size_t len = item_length(descriptor, offset, length);
        if (len == 0) {
            return false;
        }

        if (descriptor[offset] != LONG_ITEM_PREFIX) {
            if (out_count >= max_items) {
                return false;
            }
            items[out_count++] = decode_short_item(descriptor, offset);
        }
        offset += len;
    }
    return true;
}

static void register_input_report(report_accumulator (&reports)[256],
                                  uint8_t (&order)[256],
                                  uint16_t& num_reports,
                                  uint8_t report_id) {
    auto& report = reports[report_id];
    if (!report.seen) {
        report.seen = true;
        report.report_id = report_id;
        order[num_reports++] = report_id;
    }
}

static uint16_t resolved_usage_page(uint32_t usage, uint16_t default_page) {
    uint16_t page = static_cast<uint16_t>(usage >> 16);
    return page != 0 ? page : default_page;
}

static uint16_t resolved_usage_id(uint32_t usage) {
    return static_cast<uint16_t>(usage & 0xFFFFu);
}

static uint32_t resolve_variable_usage(const local_state& locals, uint32_t field_index) {
    if (field_index < locals.usage_count) {
        return locals.usages[field_index];
    }

    if (locals.has_usage_range) {
        uint32_t range_index = field_index - locals.usage_count;
        uint32_t candidate = locals.usage_minimum + range_index;
        return candidate <= locals.usage_maximum ? candidate : locals.usage_maximum;
    }

    if (locals.usage_count > 0) {
        return locals.usages[locals.usage_count - 1];
    }

    return 0;
}

static uint32_t resolve_array_usage(const local_state& locals) {
    if (locals.usage_count > 0) {
        return locals.usages[0];
    }
    if (locals.has_usage_range) {
        return locals.usage_minimum;
    }
    return 0;
}

static uint16_t build_input_flags(uint32_t raw_input_bits) {
    uint16_t flags = 0;
    if ((raw_input_bits & 0x01u) != 0) {
        flags |= input_field_constant;
    }
    if ((raw_input_bits & 0x02u) != 0) {
        flags |= input_field_variable;
    }
    if ((raw_input_bits & 0x04u) != 0) {
        flags |= input_field_relative;
    }
    return flags;
}

static int32_t analyze_input_reports(const report_item* items, size_t num_items,
                                     report_accumulator (&reports)[256],
                                     uint8_t (&order)[256],
                                     uint16_t& out_num_reports,
                                     uint32_t& out_num_fields,
                                     bool& out_uses_report_ids,
                                     uint32_t& out_max_input_report_bytes) {
    global_state globals = {};
    global_state global_stack[GLOBAL_STACK_DEPTH] = {};
    uint8_t stack_depth = 0;
    local_state locals = {};

    out_num_reports = 0;
    out_num_fields = 0;
    out_uses_report_ids = false;
    out_max_input_report_bytes = 0;

    for (size_t i = 0; i < num_items; i++) {
        const auto& item = items[i];
        switch (item.type) {
        case item_type::global:
            switch (static_cast<global_item_tag>(item.tag)) {
            case global_item_tag::usage_page:
                globals.usage_page = static_cast<uint16_t>(item.data & 0xFFFFu);
                break;
            case global_item_tag::report_size:
                globals.report_size = item.data;
                break;
            case global_item_tag::report_count:
                globals.report_count = item.data;
                break;
            case global_item_tag::report_id:
                if (item.size == 0 || item.data == 0 || item.data > 0xFFu) {
                    return -1;
                }
                if (reports[0].seen) {
                    return -1;
                }
                globals.report_id = static_cast<uint8_t>(item.data);
                out_uses_report_ids = true;
                break;
            case global_item_tag::logical_minimum:
                globals.logical_minimum = sign_extend(item.data, item.size);
                break;
            case global_item_tag::logical_maximum:
                globals.logical_maximum = sign_extend(item.data, item.size);
                break;
            case global_item_tag::push:
                if (stack_depth >= GLOBAL_STACK_DEPTH) {
                    return -1;
                }
                global_stack[stack_depth++] = globals;
                break;
            case global_item_tag::pop:
                if (stack_depth == 0) {
                    return -1;
                }
                globals = global_stack[--stack_depth];
                break;
            default:
                break;
            }
            break;

        case item_type::local:
            switch (static_cast<local_item_tag>(item.tag)) {
            case local_item_tag::usage:
                if (locals.usage_count < MAX_EXPLICIT_USAGES) {
                    locals.usages[locals.usage_count++] = item.data;
                }
                break;
            case local_item_tag::usage_minimum:
                locals.usage_minimum = item.data;
                locals.has_usage_range = true;
                break;
            case local_item_tag::usage_maximum:
                locals.usage_maximum = item.data;
                locals.has_usage_range = true;
                break;
            default:
                break;
            }
            break;

        case item_type::main:
            switch (static_cast<main_item_tag>(item.tag)) {
            case main_item_tag::input: {
                if (globals.report_size == 0 || globals.report_count == 0) {
                    return -1;
                }
                if (out_uses_report_ids && globals.report_id == 0) {
                    return -1;
                }

                uint8_t report_id = globals.report_id;
                register_input_report(reports, order, out_num_reports, report_id);

                uint64_t item_bits =
                    static_cast<uint64_t>(globals.report_size) *
                    static_cast<uint64_t>(globals.report_count);
                if (item_bits > 0xFFFFFFFFull) {
                    return -1;
                }

                auto& report = reports[report_id];
                if (report.body_bits > 0xFFFFFFFFu - static_cast<uint32_t>(item_bits)) {
                    return -1;
                }
                report.body_bits += static_cast<uint32_t>(item_bits);

                if ((item.data & 0x01u) == 0) {
                    if (report.field_count > 0xFFFFFFFFu - globals.report_count) {
                        return -1;
                    }
                    report.field_count += globals.report_count;
                    if (out_num_fields > 0xFFFFFFFFu - globals.report_count) {
                        return -1;
                    }
                    out_num_fields += globals.report_count;
                }
                locals.reset();
                break;
            }
            case main_item_tag::collection:
            case main_item_tag::end_collection:
            case main_item_tag::output:
            case main_item_tag::feature:
                locals.reset();
                break;
            default:
                break;
            }
            break;

        default:
            break;
        }
    }

    if (stack_depth != 0 || out_num_reports == 0) {
        return -1;
    }

    for (uint16_t i = 0; i < out_num_reports; i++) {
        const auto& report = reports[order[i]];
        uint32_t body_bytes = (report.body_bits + 7u) / 8u;
        uint32_t wire_bytes = body_bytes + (out_uses_report_ids ? 1u : 0u);
        if (wire_bytes > out_max_input_report_bytes) {
            out_max_input_report_bytes = wire_bytes;
        }
    }

    return 0;
}

static int32_t build_input_layout(const report_item* items, size_t num_items,
                                  const report_accumulator (&reports)[256],
                                  const uint8_t (&order)[256],
                                  uint16_t num_reports,
                                  bool uses_report_ids,
                                  report_layout& out) {
    uint16_t report_index_by_id[256];
    uint16_t next_field_cursor_by_id[256];
    uint32_t current_bit_offset_by_id[256] = {};

    for (uint16_t i = 0; i < 256; i++) {
        report_index_by_id[i] = INVALID_REPORT_INDEX;
        next_field_cursor_by_id[i] = 0;
    }

    uint32_t next_field_begin = 0;
    for (uint16_t i = 0; i < num_reports; i++) {
        const auto& acc = reports[order[i]];
        if (acc.field_count > 0xFFFFu || next_field_begin > 0xFFFFu ||
            next_field_begin + acc.field_count > 0xFFFFu) {
            return -1;
        }

        auto& report = out.input_reports[i];
        report.report_id = acc.report_id;
        report.byte_length = (acc.body_bits + 7u) / 8u;
        report.field_begin = static_cast<uint16_t>(next_field_begin);
        report.field_count = static_cast<uint16_t>(acc.field_count);
        report_index_by_id[acc.report_id] = i;
        next_field_cursor_by_id[acc.report_id] = report.field_begin;
        next_field_begin += acc.field_count;
    }

    global_state globals = {};
    global_state global_stack[GLOBAL_STACK_DEPTH] = {};
    uint8_t stack_depth = 0;
    local_state locals = {};

    for (size_t i = 0; i < num_items; i++) {
        const auto& item = items[i];
        switch (item.type) {
        case item_type::global:
            switch (static_cast<global_item_tag>(item.tag)) {
            case global_item_tag::usage_page:
                globals.usage_page = static_cast<uint16_t>(item.data & 0xFFFFu);
                break;
            case global_item_tag::report_size:
                globals.report_size = item.data;
                break;
            case global_item_tag::report_count:
                globals.report_count = item.data;
                break;
            case global_item_tag::report_id:
                if (item.size == 0 || item.data == 0 || item.data > 0xFFu) {
                    return -1;
                }
                globals.report_id = static_cast<uint8_t>(item.data);
                break;
            case global_item_tag::logical_minimum:
                globals.logical_minimum = sign_extend(item.data, item.size);
                break;
            case global_item_tag::logical_maximum:
                globals.logical_maximum = sign_extend(item.data, item.size);
                break;
            case global_item_tag::push:
                if (stack_depth >= GLOBAL_STACK_DEPTH) {
                    return -1;
                }
                global_stack[stack_depth++] = globals;
                break;
            case global_item_tag::pop:
                if (stack_depth == 0) {
                    return -1;
                }
                globals = global_stack[--stack_depth];
                break;
            default:
                break;
            }
            break;

        case item_type::local:
            switch (static_cast<local_item_tag>(item.tag)) {
            case local_item_tag::usage:
                if (locals.usage_count < MAX_EXPLICIT_USAGES) {
                    locals.usages[locals.usage_count++] = item.data;
                }
                break;
            case local_item_tag::usage_minimum:
                locals.usage_minimum = item.data;
                locals.has_usage_range = true;
                break;
            case local_item_tag::usage_maximum:
                locals.usage_maximum = item.data;
                locals.has_usage_range = true;
                break;
            default:
                break;
            }
            break;

        case item_type::main:
            switch (static_cast<main_item_tag>(item.tag)) {
            case main_item_tag::input: {
                if (globals.report_size == 0 || globals.report_count == 0 ||
                    globals.report_size > 0xFFFFu) {
                    return -1;
                }
                if (uses_report_ids && globals.report_id == 0) {
                    return -1;
                }

                uint8_t report_id = globals.report_id;
                uint16_t report_index = report_index_by_id[report_id];
                if (report_index == INVALID_REPORT_INDEX) {
                    return -1;
                }

                uint32_t item_bits = globals.report_size * globals.report_count;
                uint16_t input_flags = build_input_flags(item.data);
                bool is_constant = (input_flags & input_field_constant) != 0;
                uint32_t current_bit_offset = current_bit_offset_by_id[report_id];

                if (!is_constant) {
                    auto& report = out.input_reports[report_index];
                    uint16_t cursor = next_field_cursor_by_id[report_id];
                    uint32_t usage_template =
                        (input_flags & input_field_variable)
                            ? 0
                            : resolve_array_usage(locals);

                    for (uint32_t j = 0; j < globals.report_count; j++) {
                        if (!out.fields ||
                            cursor >= static_cast<uint16_t>(report.field_begin + report.field_count)) {
                            return -1;
                        }

                        uint32_t usage_value =
                            (input_flags & input_field_variable)
                                ? resolve_variable_usage(locals, j)
                                : usage_template;

                        out.fields[cursor++] = {
                            current_bit_offset,
                            static_cast<uint16_t>(globals.report_size),
                            report_id,
                            resolved_usage_page(usage_value, globals.usage_page),
                            resolved_usage_id(usage_value),
                            input_flags,
                            globals.logical_minimum,
                            globals.logical_maximum,
                        };
                        current_bit_offset += globals.report_size;
                    }

                    next_field_cursor_by_id[report_id] = cursor;
                } else {
                    current_bit_offset += item_bits;
                }

                current_bit_offset_by_id[report_id] = current_bit_offset;
                locals.reset();
                break;
            }
            case main_item_tag::collection:
            case main_item_tag::end_collection:
            case main_item_tag::output:
            case main_item_tag::feature:
                locals.reset();
                break;
            default:
                break;
            }
            break;

        default:
            break;
        }
    }

    if (stack_depth != 0) {
        return -1;
    }

    for (uint16_t i = 0; i < num_reports; i++) {
        uint8_t report_id = order[i];
        if (current_bit_offset_by_id[report_id] != reports[report_id].body_bits) {
            return -1;
        }
    }

    return 0;
}

void report_layout::destroy() {
    if (fields) {
        heap::ufree(fields);
        fields = nullptr;
    }
    if (input_reports) {
        heap::ufree(input_reports);
        input_reports = nullptr;
    }
    num_fields = 0;
    num_input_reports = 0;
    uses_report_ids = false;
    max_input_report_bytes = 0;
}

const input_report_info* find_input_report(const report_layout& layout, uint8_t report_id) {
    for (uint16_t i = 0; i < layout.num_input_reports; i++) {
        if (layout.input_reports[i].report_id == report_id) {
            return &layout.input_reports[i];
        }
    }
    return nullptr;
}

int32_t parse_report_descriptor(const uint8_t* descriptor, size_t length,
                                report_layout& out) {
    out.destroy();

    if (!descriptor || length == 0) {
        return -1;
    }

    size_t item_count = 0;
    if (!count_items(descriptor, length, item_count) || item_count == 0) {
        return -1;
    }

    auto* items = static_cast<report_item*>(
        heap::ualloc(item_count * sizeof(report_item)));
    if (!items) {
        return -1;
    }

    size_t parsed_count = 0;
    if (!parse_items(descriptor, length, items, item_count, parsed_count) ||
        parsed_count != item_count) {
        heap::ufree(items);
        return -1;
    }

    report_accumulator reports[256] = {};
    uint8_t report_order[256] = {};
    uint16_t num_reports = 0;
    uint32_t num_fields = 0;
    bool uses_report_ids = false;
    uint32_t max_input_report_bytes = 0;

    int32_t rc = analyze_input_reports(items, parsed_count, reports, report_order,
                                       num_reports, num_fields,
                                       uses_report_ids, max_input_report_bytes);
    if (rc != 0 || num_reports == 0 || num_fields > 0xFFFFu) {
        heap::ufree(items);
        return -1;
    }

    out.input_reports = static_cast<input_report_info*>(
        heap::ualloc(num_reports * sizeof(input_report_info)));
    if (!out.input_reports) {
        heap::ufree(items);
        return -1;
    }

    if (num_fields > 0) {
        out.fields = static_cast<field_info*>(
            heap::ualloc(num_fields * sizeof(field_info)));
        if (!out.fields) {
            heap::ufree(items);
            out.destroy();
            return -1;
        }
    }

    out.num_fields = static_cast<uint16_t>(num_fields);
    out.num_input_reports = num_reports;
    out.uses_report_ids = uses_report_ids;
    out.max_input_report_bytes = max_input_report_bytes;

    rc = build_input_layout(items, parsed_count, reports, report_order,
                            num_reports, uses_report_ids, out);
    heap::ufree(items);
    if (rc != 0) {
        out.destroy();
        return -1;
    }

    return 0;
}

} // namespace usb::hid
