#include "debug/dwarf_line.h"
#include "debug/leb128.h"
#include "common/string.h"
#include "common/logging.h"
#include "mm/heap.h"

namespace dwarf_line {

constexpr uint8_t DW_LNS_copy             = 1;
constexpr uint8_t DW_LNS_advance_pc       = 2;
constexpr uint8_t DW_LNS_advance_line     = 3;
constexpr uint8_t DW_LNS_set_file         = 4;
constexpr uint8_t DW_LNS_set_column       = 5;
constexpr uint8_t DW_LNS_negate_stmt      = 6;
constexpr uint8_t DW_LNS_set_basic_block  = 7;
constexpr uint8_t DW_LNS_const_add_pc     = 8;
constexpr uint8_t DW_LNS_fixed_advance_pc = 9;

constexpr uint8_t DW_LNE_end_sequence      = 1;
constexpr uint8_t DW_LNE_set_address       = 2;
constexpr uint8_t DW_LNE_set_discriminator = 4;

constexpr uint64_t DW_LNCT_path            = 0x01;
constexpr uint64_t DW_LNCT_directory_index = 0x02;

constexpr uint64_t DW_FORM_data1     = 0x0b;
constexpr uint64_t DW_FORM_data2     = 0x05;
constexpr uint64_t DW_FORM_data16    = 0x1e;
constexpr uint64_t DW_FORM_string    = 0x08;
constexpr uint64_t DW_FORM_line_strp = 0x1f;
constexpr uint64_t DW_FORM_udata     = 0x0f;
constexpr uint64_t DW_FORM_strp      = 0x11;

static const uint8_t* g_debug_line      = nullptr;
static uint64_t       g_debug_line_size = 0;
static const char*    g_line_str        = nullptr;
static uint64_t       g_line_str_size   = 0;
static bool           g_available       = false;

constexpr uint64_t MAX_FILES = 512;
constexpr uint64_t MAX_DIRS  = 64;
constexpr uint64_t MAX_WALK_BYTES = 256 * 1024;

struct format_entry { uint64_t content_type; uint64_t form; };

static inline uint8_t read_u8(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return 0;
    return *p++;
}

static inline uint16_t read_u16(const uint8_t*& p, const uint8_t* end) {
    if (p + 2 > end) { p = end; return 0; }
    uint16_t v = p[0] | (static_cast<uint16_t>(p[1]) << 8);
    p += 2;
    return v;
}

static inline uint32_t read_u32(const uint8_t*& p, const uint8_t* end) {
    if (p + 4 > end) { p = end; return 0; }
    uint32_t v = p[0] | (static_cast<uint32_t>(p[1]) << 8)
               | (static_cast<uint32_t>(p[2]) << 16)
               | (static_cast<uint32_t>(p[3]) << 24);
    p += 4;
    return v;
}

static inline uint64_t read_u64(const uint8_t*& p, const uint8_t* end) {
    if (p + 8 > end) { p = end; return 0; }
    uint64_t lo = read_u32(p, end);
    uint64_t hi = read_u32(p, end);
    return lo | (hi << 32);
}

static const char* read_form_string(const uint8_t*& p, const uint8_t* end,
                                    uint64_t form) {
    switch (form) {
        case DW_FORM_string: {
            const char* s = reinterpret_cast<const char*>(p);
            while (p < end && *p != 0) p++;
            if (p < end) p++;
            return s;
        }
        case DW_FORM_line_strp: {
            uint32_t off = read_u32(p, end);
            if (g_line_str && off < g_line_str_size)
                return g_line_str + off;
            return "(unknown)";
        }
        case DW_FORM_strp: {
            read_u32(p, end);
            return "(debug_str)";
        }
        default:
            return "(unknown)";
    }
}

static uint64_t read_form_uint(const uint8_t*& p, const uint8_t* end,
                               uint64_t form) {
    switch (form) {
        case DW_FORM_data1: return read_u8(p, end);
        case DW_FORM_data2: return read_u16(p, end);
        case DW_FORM_udata: return leb128::read_uleb128(p, end);
        default:            return 0;
    }
}

static void skip_form(const uint8_t*& p, const uint8_t* end, uint64_t form) {
    switch (form) {
        case DW_FORM_data1:     if (p < end) p += 1; break;
        case DW_FORM_data2:     if (p + 2 <= end) p += 2; break;
        case DW_FORM_data16:    if (p + 16 <= end) p += 16; break;
        case DW_FORM_udata:     leb128::read_uleb128(p, end); break;
        case DW_FORM_line_strp: if (p + 4 <= end) p += 4; break;
        case DW_FORM_strp:      if (p + 4 <= end) p += 4; break;
        case DW_FORM_string:
            while (p < end && *p != 0) p++;
            if (p < end) p++;
            break;
        default: break;
    }
}

struct line_unit_context {
    const char* dirs[MAX_DIRS];
    const char* file_names[MAX_FILES];
    uint64_t    file_dir_idx[MAX_FILES];
    uint64_t    dir_count;
    uint64_t    file_count;
    uint8_t  min_inst_length;
    uint8_t  max_ops_per_inst;
    int8_t   line_base;
    uint8_t  line_range;
    uint8_t  opcode_base;
    uint8_t  default_is_stmt;
    uint8_t  address_size;
    uint8_t  std_opcode_lengths[13];
};

static bool parse_unit_header(const uint8_t*& p, const uint8_t* unit_end,
                              line_unit_context& ctx) {
    const uint8_t* end = unit_end;
    uint16_t version = read_u16(p, end);
    if (version != 5) return false;

    ctx.address_size = read_u8(p, end);
    read_u8(p, end);

    uint32_t header_length = read_u32(p, end);
    const uint8_t* program_start = p + header_length;

    ctx.min_inst_length  = read_u8(p, end);
    ctx.max_ops_per_inst = read_u8(p, end);
    ctx.default_is_stmt  = read_u8(p, end);
    ctx.line_base        = static_cast<int8_t>(read_u8(p, end));
    ctx.line_range       = read_u8(p, end);
    ctx.opcode_base      = read_u8(p, end);

    for (uint8_t i = 1; i < ctx.opcode_base && i < 13; i++)
        ctx.std_opcode_lengths[i] = read_u8(p, end);

    uint8_t dir_fmt_count = read_u8(p, end);
    format_entry dir_fmts[8];
    for (uint8_t i = 0; i < dir_fmt_count && i < 8; i++) {
        dir_fmts[i].content_type = leb128::read_uleb128(p, end);
        dir_fmts[i].form         = leb128::read_uleb128(p, end);
    }

    ctx.dir_count = leb128::read_uleb128(p, end);
    if (ctx.dir_count > MAX_DIRS) ctx.dir_count = MAX_DIRS;

    for (uint64_t d = 0; d < ctx.dir_count; d++) {
        ctx.dirs[d] = nullptr;
        for (uint8_t f = 0; f < dir_fmt_count; f++) {
            if (dir_fmts[f].content_type == DW_LNCT_path)
                ctx.dirs[d] = read_form_string(p, end, dir_fmts[f].form);
            else
                skip_form(p, end, dir_fmts[f].form);
        }
    }

    uint8_t file_fmt_count = read_u8(p, end);
    format_entry file_fmts[8];
    for (uint8_t i = 0; i < file_fmt_count && i < 8; i++) {
        file_fmts[i].content_type = leb128::read_uleb128(p, end);
        file_fmts[i].form         = leb128::read_uleb128(p, end);
    }

    ctx.file_count = leb128::read_uleb128(p, end);
    if (ctx.file_count > MAX_FILES) ctx.file_count = MAX_FILES;

    for (uint64_t fi = 0; fi < ctx.file_count; fi++) {
        ctx.file_names[fi] = nullptr;
        ctx.file_dir_idx[fi] = 0;
        for (uint8_t f = 0; f < file_fmt_count; f++) {
            if (file_fmts[f].content_type == DW_LNCT_path)
                ctx.file_names[fi] = read_form_string(p, end, file_fmts[f].form);
            else if (file_fmts[f].content_type == DW_LNCT_directory_index)
                ctx.file_dir_idx[fi] = read_form_uint(p, end, file_fmts[f].form);
            else
                skip_form(p, end, file_fmts[f].form);
        }
    }

    p = program_start;
    return true;
}

static const char* get_file_name(const line_unit_context& ctx, uint64_t idx) {
    if (idx >= ctx.file_count) return nullptr;
    return ctx.file_names[idx];
}

static bool run_line_program(const uint8_t* prog, const uint8_t* end,
                             const line_unit_context& ctx,
                             uint64_t target, resolve_result* out) {
    uint64_t address = 0;
    int64_t  line    = 1;
    uint64_t file    = 0;
    uint64_t prev_addr = 0;
    int64_t  prev_line = 1;
    uint64_t prev_file = 0;
    bool     has_prev  = false;

    const uint8_t* p = prog;
    const uint8_t* limit = prog + MAX_WALK_BYTES;
    if (limit > end) limit = end;

    while (p < end && p < limit) {
        uint8_t op = read_u8(p, end);

        if (op == 0) {
            uint64_t ext_len = leb128::read_uleb128(p, end);
            const uint8_t* ext_end = p + ext_len;
            if (ext_end > end) ext_end = end;
            uint8_t ext_op = read_u8(p, ext_end);

            switch (ext_op) {
                case DW_LNE_end_sequence:
                    if (has_prev && prev_addr <= target && target < address) {
                        out->file = get_file_name(ctx, prev_file);
                        out->line = static_cast<uint32_t>(prev_line);
                        return out->file != nullptr;
                    }
                    address = 0; line = 1; file = 0;
                    has_prev = false;
                    break;
                case DW_LNE_set_address:
                    address = (ctx.address_size == 8)
                        ? read_u64(p, ext_end)
                        : read_u32(p, ext_end);
                    break;
                case DW_LNE_set_discriminator:
                    leb128::read_uleb128(p, ext_end);
                    break;
                default: break;
            }
            p = ext_end;

        } else if (op < ctx.opcode_base) {
            switch (op) {
                case DW_LNS_copy:
                    if (has_prev && prev_addr <= target && target < address) {
                        out->file = get_file_name(ctx, prev_file);
                        out->line = static_cast<uint32_t>(prev_line);
                        return out->file != nullptr;
                    }
                    prev_addr = address;
                    prev_line = line;
                    prev_file = file;
                    has_prev  = true;
                    break;
                case DW_LNS_advance_pc:
                    address += ctx.min_inst_length * leb128::read_uleb128(p, end);
                    break;
                case DW_LNS_advance_line:
                    line += leb128::read_sleb128(p, end);
                    break;
                case DW_LNS_set_file:
                    file = leb128::read_uleb128(p, end);
                    break;
                case DW_LNS_set_column:
                    leb128::read_uleb128(p, end);
                    break;
                case DW_LNS_negate_stmt:
                case DW_LNS_set_basic_block:
                    break;
                case DW_LNS_const_add_pc: {
                    uint64_t adj = (255 - ctx.opcode_base) / ctx.line_range;
                    address += ctx.min_inst_length * adj;
                    break;
                }
                case DW_LNS_fixed_advance_pc:
                    address += read_u16(p, end);
                    break;
                default:
                    if (op < 13) {
                        for (uint8_t k = 0; k < ctx.std_opcode_lengths[op]; k++)
                            leb128::read_uleb128(p, end);
                    }
                    break;
            }
        } else {
            uint8_t adjusted = op - ctx.opcode_base;
            uint64_t addr_adv = adjusted / ctx.line_range;
            int64_t  line_adv = ctx.line_base + (adjusted % ctx.line_range);

            if (has_prev && prev_addr <= target && target < address) {
                out->file = get_file_name(ctx, prev_file);
                out->line = static_cast<uint32_t>(prev_line);
                return out->file != nullptr;
            }
            prev_addr = address;
            prev_line = line;
            prev_file = file;
            has_prev  = true;

            address += ctx.min_inst_length * addr_adv;
            line    += line_adv;
        }
    }

    if (has_prev && prev_addr <= target) {
        out->file = get_file_name(ctx, prev_file);
        out->line = static_cast<uint32_t>(prev_line);
        return out->file != nullptr;
    }
    return false;
}

__PRIVILEGED_CODE int32_t init(const debug::kernel_elf& elf) {
    const elf64::Shdr* line_shdr = elf.find_section(".debug_line");
    if (!line_shdr) return ERR_NO_SECTION;

    if (line_shdr->sh_offset + line_shdr->sh_size > elf.file_size)
        return ERR_BAD_DWARF;

    uint64_t line_size = line_shdr->sh_size;
    auto* line_copy = reinterpret_cast<uint8_t*>(heap::kalloc(line_size));
    if (!line_copy) return ERR_NO_MEMORY;
    string::memcpy(line_copy, elf.base + line_shdr->sh_offset, line_size);

    const elf64::Shdr* str_shdr = elf.find_section(".debug_line_str");
    char* str_copy = nullptr;
    uint64_t str_size = 0;
    if (str_shdr && str_shdr->sh_offset + str_shdr->sh_size <= elf.file_size) {
        str_size = str_shdr->sh_size;
        str_copy = reinterpret_cast<char*>(heap::kalloc(str_size));
        if (str_copy)
            string::memcpy(str_copy, elf.base + str_shdr->sh_offset, str_size);
    }

    g_debug_line      = line_copy;
    g_debug_line_size = line_size;
    g_line_str        = str_copy;
    g_line_str_size   = str_size;
    g_available       = true;

    log::info("dwarf_line: loaded .debug_line (%lu bytes)", line_size);
    return OK;
}

bool resolve(uint64_t addr, resolve_result* out) {
    if (!g_available || !out) return false;

    const uint8_t* p   = g_debug_line;
    const uint8_t* end = g_debug_line + g_debug_line_size;

    while (p < end) {
        uint32_t unit_length = read_u32(p, end);
        if (unit_length == 0) break;

        const uint8_t* unit_end = p + unit_length;
        if (unit_end > end) unit_end = end;

        line_unit_context ctx;
        string::memset(&ctx, 0, sizeof(ctx));

        if (!parse_unit_header(p, unit_end, ctx)) {
            p = unit_end;
            continue;
        }

        if (run_line_program(p, unit_end, ctx, addr, out))
            return true;

        p = unit_end;
    }
    return false;
}

} // namespace dwarf_line
