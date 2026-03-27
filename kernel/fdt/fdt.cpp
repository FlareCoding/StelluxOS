#include "fdt/fdt.h"
#include "boot/boot_services.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "common/logging.h"
#include "common/string.h"

namespace fdt {

static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

static inline uint32_t align4(uint32_t v) { return (v + 3) & ~3u; }

__PRIVILEGED_DATA static bool g_initialized = false;
__PRIVILEGED_DATA static const uint8_t* g_base = nullptr;
__PRIVILEGED_DATA static const uint8_t* g_struct_base = nullptr;
__PRIVILEGED_DATA static const char* g_strings_base = nullptr;
__PRIVILEGED_DATA static uint32_t g_struct_size = 0;
__PRIVILEGED_DATA static uint32_t g_strings_size = 0;

static const char* get_string(uint32_t nameoff) {
    if (nameoff >= g_strings_size) return "";
    return g_strings_base + nameoff;
}

static uint32_t read_token(uint32_t offset) {
    if (offset + 4 > g_struct_size) return FDT_END;
    uint32_t raw;
    string::memcpy(&raw, g_struct_base + offset, 4);
    return be32(raw);
}

static uint32_t read_cell32(const uint8_t* p) {
    uint32_t raw;
    string::memcpy(&raw, p, 4);
    return be32(raw);
}

// Check if the NUL-separated compatible list contains the target string.
static bool compatible_match(const uint8_t* data, uint32_t len, const char* target) {
    uint32_t tlen = string::strlen(target);
    uint32_t pos = 0;
    while (pos < len) {
        const char* entry = reinterpret_cast<const char*>(data + pos);
        uint32_t elen = string::strnlen(entry, len - pos);
        if (elen == tlen && string::memcmp(entry, target, tlen) == 0) return true;
        pos += elen + 1;
    }
    return false;
}

// Get a property from the node at node_offset (pointing to FDT_BEGIN_NODE).
// Returns pointer to property data and sets out_len, or nullptr if not found.
static const void* node_get_prop(uint32_t node_offset, const char* name, uint32_t* out_len) {
    if (read_token(node_offset) != FDT_BEGIN_NODE) return nullptr;
    uint32_t offset = node_offset + 4;

    // Skip name
    while (offset < g_struct_size && g_struct_base[offset] != 0) offset++;
    offset = align4(offset + 1);

    // Walk properties (all props come before child nodes per spec)
    while (offset < g_struct_size) {
        uint32_t tok = read_token(offset);
        if (tok == FDT_NOP) { offset += 4; continue; }
        if (tok != FDT_PROP) break;
        offset += 4;

        if (offset + 8 > g_struct_size) return nullptr;
        uint32_t len = read_cell32(g_struct_base + offset);
        uint32_t nameoff = read_cell32(g_struct_base + offset + 4);
        offset += 8;

        const char* pname = get_string(nameoff);
        if (string::strcmp(pname, name) == 0) {
            if (out_len) *out_len = len;
            return g_struct_base + offset;
        }
        offset = align4(offset + len);
    }
    return nullptr;
}

// Read a uint32_t property value (e.g., #address-cells).
static uint32_t node_get_u32(uint32_t node_offset, const char* name, uint32_t default_val) {
    uint32_t len = 0;
    const void* data = node_get_prop(node_offset, name, &len);
    if (!data || len < 4) return default_val;
    return read_cell32(static_cast<const uint8_t*>(data));
}

__PRIVILEGED_CODE int32_t init() {
    if (g_initialized) return OK;

    if (g_boot_info.dtb_phys == 0 || g_boot_info.dtb_size == 0) {
        return ERR_NO_DTB;
    }

    // Map the DTB into kernel VA (may be in non-USABLE memory)
    uintptr_t map_base = 0, map_va = 0;
    int32_t rc = vmm::map_phys(
        g_boot_info.dtb_phys, g_boot_info.dtb_size,
        paging::PAGE_READ,
        map_base, map_va);
    if (rc != vmm::OK) {
        log::warn("fdt: failed to map DTB at phys 0x%lx (%d)", g_boot_info.dtb_phys, rc);
        return ERR_NO_DTB;
    }

    g_base = reinterpret_cast<const uint8_t*>(map_va);
    auto* hdr = reinterpret_cast<const fdt_header*>(g_base);

    if (be32(hdr->magic) != FDT_MAGIC) {
        log::warn("fdt: bad magic 0x%08x", be32(hdr->magic));
        return ERR_BAD_MAGIC;
    }

    uint32_t total = be32(hdr->totalsize);
    if (total < sizeof(fdt_header) || total > g_boot_info.dtb_size) {
        log::warn("fdt: invalid totalsize %u", total);
        return ERR_BAD_DATA;
    }

    g_struct_base = g_base + be32(hdr->off_dt_struct);
    g_strings_base = reinterpret_cast<const char*>(g_base + be32(hdr->off_dt_strings));
    g_struct_size = be32(hdr->size_dt_struct);
    g_strings_size = be32(hdr->size_dt_strings);

    log::info("fdt: DTB at phys 0x%lx, version %u, size %u bytes",
              g_boot_info.dtb_phys, be32(hdr->version), total);

    g_initialized = true;
    return OK;
}

__PRIVILEGED_CODE int32_t find_compatible(const char* compatible) {
    if (!g_initialized) return ERR_NO_DTB;

    uint32_t offset = 0;

    while (offset < g_struct_size) {
        uint32_t tok = read_token(offset);

        if (tok == FDT_BEGIN_NODE) {
            uint32_t node_start = offset;

            uint32_t clen = 0;
            const void* cdata = node_get_prop(offset, "compatible", &clen);
            if (cdata && clen > 0) {
                if (compatible_match(static_cast<const uint8_t*>(cdata), clen, compatible)) {
                    return static_cast<int32_t>(node_start);
                }
            }

            // No match -- advance past node name and descend into children
            offset += 4;
            while (offset < g_struct_size && g_struct_base[offset] != 0) offset++;
            offset = align4(offset + 1);
            continue;
        }

        if (tok == FDT_END_NODE || tok == FDT_NOP) {
            offset += 4;
            continue;
        }

        if (tok == FDT_PROP) {
            offset += 4;
            if (offset + 8 > g_struct_size) break;
            uint32_t len = read_cell32(g_struct_base + offset);
            offset += 8;
            offset = align4(offset + len);
            continue;
        }

        if (tok == FDT_END) break;
        break;
    }

    return ERR_NOT_FOUND;
}

__PRIVILEGED_CODE const void* get_prop(int32_t node_offset, const char* name, uint32_t* out_len) {
    if (!g_initialized || node_offset < 0) return nullptr;
    return node_get_prop(static_cast<uint32_t>(node_offset), name, out_len);
}

__PRIVILEGED_CODE int32_t get_reg(int32_t node_offset, uint64_t* out_base, uint64_t* out_size) {
    if (!g_initialized || node_offset < 0) return ERR_NO_DTB;
    if (!out_base || !out_size) return ERR_BAD_DATA;

    uint32_t target = static_cast<uint32_t>(node_offset);

    // Simple approach: walk from root, find the direct parent of our node
    // The parent's properties define how to interpret our reg
    // Default values per DT spec
    uint32_t parent_addr_cells = 2;
    uint32_t parent_size_cells = 1;

    // Walk from root to find parent
    uint32_t offset = 0;
    uint32_t depth = 0;
    constexpr uint32_t MAX_DEPTH = 16;
    uint32_t parent_stack[MAX_DEPTH] = {};

    while (offset < g_struct_size && offset <= target) {
        uint32_t tok = read_token(offset);

        if (tok == FDT_BEGIN_NODE) {
            if (depth < MAX_DEPTH) parent_stack[depth] = offset;
            depth++;
            if (offset == target) break;
            offset += 4;
            while (offset < g_struct_size && g_struct_base[offset] != 0) offset++;
            offset = align4(offset + 1);
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (depth > 0) depth--;
            offset += 4;
            continue;
        }
        if (tok == FDT_PROP) {
            offset += 4;
            if (offset + 8 > g_struct_size) return ERR_BAD_DATA;
            uint32_t len = read_cell32(g_struct_base + offset);
            offset += 8;
            offset = align4(offset + len);
            continue;
        }
        if (tok == FDT_NOP) { offset += 4; continue; }
        break;
    }

    // Parent is at depth-1
    if (depth >= 2) {
        uint32_t poff = parent_stack[depth - 2];
        parent_addr_cells = node_get_u32(poff, "#address-cells", 2);
        parent_size_cells = node_get_u32(poff, "#size-cells", 1);
    }

    // Read the reg property from our target node
    uint32_t reg_len = 0;
    const void* reg_data = node_get_prop(target, "reg", &reg_len);
    if (!reg_data) return ERR_NOT_FOUND;

    const uint8_t* cells = static_cast<const uint8_t*>(reg_data);
    uint32_t needed = (parent_addr_cells + parent_size_cells) * 4;
    if (reg_len < needed) return ERR_BAD_DATA;

    // Build address from cells
    uint64_t addr = 0;
    for (uint32_t i = 0; i < parent_addr_cells; i++) {
        addr = (addr << 32) | read_cell32(cells + i * 4);
    }

    // Build size from cells
    uint64_t size = 0;
    for (uint32_t i = 0; i < parent_size_cells; i++) {
        size = (size << 32) | read_cell32(cells + (parent_addr_cells + i) * 4);
    }

    // Try single-level address translation via parent's ranges
    if (depth >= 2) {
        uint32_t poff = parent_stack[depth - 2];
        uint32_t ranges_len = 0;
        const void* ranges_data = node_get_prop(poff, "ranges", &ranges_len);

        if (ranges_data && ranges_len > 0) {
            // Parent's ranges: each entry is (child_addr[ac] + parent_addr[pac] + size[sc])
            // where ac = parent's #address-cells, pac = grandparent's #address-cells, sc = parent's #size-cells
            uint32_t gp_addr_cells = 2; // default for grandparent
            if (depth >= 3) {
                gp_addr_cells = node_get_u32(parent_stack[depth - 3], "#address-cells", 2);
            }

            uint32_t entry_cells = parent_addr_cells + gp_addr_cells + parent_size_cells;
            uint32_t entry_bytes = entry_cells * 4;
            const uint8_t* rp = static_cast<const uint8_t*>(ranges_data);

            for (uint32_t roff = 0; roff + entry_bytes <= ranges_len; roff += entry_bytes) {
                // Child base
                uint64_t child_base = 0;
                for (uint32_t i = 0; i < parent_addr_cells; i++) {
                    child_base = (child_base << 32) | read_cell32(rp + roff + i * 4);
                }

                // Parent (CPU) base
                uint64_t cpu_base = 0;
                for (uint32_t i = 0; i < gp_addr_cells; i++) {
                    cpu_base = (cpu_base << 32) | read_cell32(rp + roff + parent_addr_cells * 4 + i * 4);
                }

                // Range size
                uint64_t range_size = 0;
                for (uint32_t i = 0; i < parent_size_cells; i++) {
                    range_size = (range_size << 32) | read_cell32(
                        rp + roff + (parent_addr_cells + gp_addr_cells) * 4 + i * 4);
                }

                // Check if our address falls in this range
                if (addr >= child_base && addr < child_base + range_size) {
                    addr = cpu_base + (addr - child_base);
                    break;
                }
            }
        }
    }

    *out_base = addr;
    *out_size = size;
    return OK;
}

__PRIVILEGED_CODE int32_t get_interrupts(int32_t node_offset,
                                         uint32_t* out_irqs, uint32_t max_irqs) {
    if (!g_initialized || node_offset < 0) return ERR_NO_DTB;
    if (!out_irqs || max_irqs == 0) return ERR_BAD_DATA;

    uint32_t len = 0;
    const void* data = node_get_prop(static_cast<uint32_t>(node_offset),
                                     "interrupts", &len);
    if (!data) return ERR_NOT_FOUND;

    // GIC interrupt cells: 3 cells each (type, number, flags).
    // SPI N maps to GIC IRQ (N + 32), PPI N maps to (N + 16).
    constexpr uint32_t GIC_SPI_OFFSET = 32;
    constexpr uint32_t CELLS_PER_IRQ = 3;
    constexpr uint32_t BYTES_PER_IRQ = CELLS_PER_IRQ * 4;

    const uint8_t* cells = static_cast<const uint8_t*>(data);
    uint32_t count = 0;

    for (uint32_t off = 0; off + BYTES_PER_IRQ <= len && count < max_irqs;
         off += BYTES_PER_IRQ) {
        uint32_t type = read_cell32(cells + off);
        uint32_t num  = read_cell32(cells + off + 4);
        // uint32_t flags = read_cell32(cells + off + 8);  // unused for now

        if (type == 0) {
            // SPI interrupt
            out_irqs[count++] = num + GIC_SPI_OFFSET;
        } else if (type == 1) {
            // PPI interrupt
            out_irqs[count++] = num + 16;
        }
    }

    return static_cast<int32_t>(count);
}

} // namespace fdt
