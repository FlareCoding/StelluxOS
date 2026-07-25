#include "drivers/gpu/nvidia/nv_firmware.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace nvidia {

// ---- little-endian buffer readers -----------------------------------------
static uint16_t rd16(const uint8_t* b, uint64_t o) {
    return static_cast<uint16_t>(b[o]) |
           static_cast<uint16_t>(static_cast<uint16_t>(b[o + 1]) << 8);
}
static uint32_t rd32(const uint8_t* b, uint64_t o) {
    return static_cast<uint32_t>(b[o]) |
           (static_cast<uint32_t>(b[o + 1]) << 8) |
           (static_cast<uint32_t>(b[o + 2]) << 16) |
           (static_cast<uint32_t>(b[o + 3]) << 24);
}
static uint64_t rd64(const uint8_t* b, uint64_t o) {
    return static_cast<uint64_t>(rd32(b, o)) |
           (static_cast<uint64_t>(rd32(b, o + 4)) << 32);
}

// ---- tiny string helpers (avoid pulling in libc semantics) ----------------
static bool str_eq(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}
static bool str_prefix(const char* s, const char* p) {
    while (*p) { if (*s != *p) return false; s++; p++; }
    return true;
}

// Small zeroed scratch buffer from the unprivileged heap (PAGE_USER_RW): the ELF section headers /
// string table parsed here are touched from the ring-3 driver task. uzalloc auto-elevates internally.
static uint8_t* kbuf(size_t n) {
    return static_cast<uint8_t*>(heap::uzalloc(n));
}
static void kbuf_free(void* p) {
    if (p) { heap::ufree(p); }
}

// Seek to off and read exactly len bytes.
static bool read_at(fs::file* f, uint64_t off, void* buf, size_t len) {
    if (fs::seek(f, static_cast<int64_t>(off), fs::SEEK_SET) < 0) return false;
    ssize_t got = fs::read(f, buf, len);
    return got >= 0 && static_cast<size_t>(got) == len;
}

// ELF64 constants of interest
constexpr uint8_t  ELFCLASS64 = 2;
constexpr uint16_t EM_RISCV   = 243;
constexpr uint16_t SHDR_SIZE  = 64;   // Elf64_Shdr size
constexpr uint32_t MAX_SHDRS  = 64;
constexpr uint64_t MAX_SHSTR  = 65536;

int32_t fw_probe_gsp(gsp_fw_image& out) {
    // zero-initialize the result
    for (uint32_t i = 0; i < (sizeof(out.fwsig_off) / sizeof(out.fwsig_off[0])); i++) {
        out.fwsig_off[i] = 0;
        out.fwsig_size[i] = 0;
    }
    out.fwimage_off = 0; out.fwimage_size = 0;
    out.fwversion_off = 0; out.fwversion_size = 0;
    out.fwsig_count = 0;
    out.sig_ga10x_off = 0; out.sig_ga10x_size = 0;
    for (uint32_t i = 0; i < sizeof(out.version); i++) out.version[i] = 0;

    log::info("nvidia: fw: opening %s", GSP_FW_PATH);

    fs::vattr attr;
    if (fs::stat(GSP_FW_PATH, &attr) != fs::OK) {
        log::error("nvidia: fw: stat failed - is the firmware staged in the initrd?");
        return ERR_FW_NOENT;
    }
    log::info("nvidia: fw: file size = %lu bytes (%lu MiB)",
              static_cast<unsigned long>(attr.size),
              static_cast<unsigned long>(attr.size >> 20));

    int32_t oerr = 0;
    fs::file* f = fs::open(GSP_FW_PATH, fs::O_RDONLY, &oerr);
    if (!f) {
        log::error("nvidia: fw: open failed err=%d", oerr);
        return ERR_FW_NOENT;
    }

    int32_t result = ERR_FW_FORMAT;
    uint8_t* shdrs = nullptr;
    uint8_t* shstr = nullptr;

    do {
        uint8_t eh[64];
        if (!read_at(f, 0, eh, sizeof(eh))) {
            log::error("nvidia: fw: cannot read ELF header");
            result = ERR_FW_IO;
            break;
        }
        if (!(eh[0] == 0x7f && eh[1] == 'E' && eh[2] == 'L' && eh[3] == 'F')) {
            log::error("nvidia: fw: bad ELF magic %02x %02x %02x %02x",
                       eh[0], eh[1], eh[2], eh[3]);
            break;
        }

        const uint8_t  ei_class    = eh[4];
        const uint16_t e_type      = rd16(eh, 0x10);
        const uint16_t e_machine   = rd16(eh, 0x12);
        const uint64_t e_shoff     = rd64(eh, 0x28);
        const uint16_t e_shentsize = rd16(eh, 0x3a);
        const uint16_t e_shnum     = rd16(eh, 0x3c);
        const uint16_t e_shstrndx  = rd16(eh, 0x3e);

        log::info("nvidia: fw: ELF class=%u type=%u machine=%u shoff=0x%lx shnum=%u shent=%u shstrndx=%u",
                  ei_class, e_type, e_machine,
                  static_cast<unsigned long>(e_shoff), e_shnum, e_shentsize, e_shstrndx);

        if (ei_class != ELFCLASS64 || e_machine != EM_RISCV) {
            log::error("nvidia: fw: unexpected ELF (want 64-bit RISC-V)");
            break;
        }
        if (e_shentsize != SHDR_SIZE || e_shnum == 0 || e_shnum > MAX_SHDRS ||
            e_shstrndx >= e_shnum) {
            log::error("nvidia: fw: unexpected section-table geometry");
            break;
        }

        const size_t shdr_bytes = static_cast<size_t>(e_shnum) * SHDR_SIZE;
        shdrs = kbuf(shdr_bytes);
        if (!shdrs) { result = ERR_FW_IO; break; }
        if (!read_at(f, e_shoff, shdrs, shdr_bytes)) {
            log::error("nvidia: fw: cannot read section headers");
            result = ERR_FW_IO;
            break;
        }

        // Section-name string table (shstrtab).
        const uint8_t* sstr_hdr = shdrs + static_cast<size_t>(e_shstrndx) * SHDR_SIZE;
        const uint64_t sstr_off = rd64(sstr_hdr, 0x18);
        const uint64_t sstr_sz  = rd64(sstr_hdr, 0x20);
        if (sstr_sz == 0 || sstr_sz > MAX_SHSTR) {
            log::error("nvidia: fw: bad shstrtab size %lu", static_cast<unsigned long>(sstr_sz));
            break;
        }
        shstr = kbuf(static_cast<size_t>(sstr_sz) + 1); // +1 guarantees NUL terminator
        if (!shstr) { result = ERR_FW_IO; break; }
        if (!read_at(f, sstr_off, shstr, sstr_sz)) {
            log::error("nvidia: fw: cannot read shstrtab");
            result = ERR_FW_IO;
            break;
        }

        // Walk sections, recording the ones we care about.
        for (uint16_t i = 0; i < e_shnum; i++) {
            const uint8_t* sh = shdrs + static_cast<size_t>(i) * SHDR_SIZE;
            const uint32_t sh_name = rd32(sh, 0x00);
            const uint64_t sh_off  = rd64(sh, 0x18);
            const uint64_t sh_size = rd64(sh, 0x20);
            if (sh_name >= sstr_sz) continue;
            const char* name = reinterpret_cast<const char*>(shstr + sh_name);

            if (str_eq(name, ".fwimage")) {
                out.fwimage_off = sh_off;
                out.fwimage_size = sh_size;
                log::info("nvidia: fw:   .fwimage      off=0x%lx size=%lu",
                          static_cast<unsigned long>(sh_off),
                          static_cast<unsigned long>(sh_size));
            } else if (str_eq(name, ".fwversion")) {
                out.fwversion_off = sh_off;
                out.fwversion_size = sh_size;
                uint8_t vbuf[32];
                for (uint32_t k = 0; k < sizeof(vbuf); k++) vbuf[k] = 0;
                size_t vlen = (sh_size < sizeof(vbuf) - 1)
                                  ? static_cast<size_t>(sh_size)
                                  : sizeof(vbuf) - 1;
                if (read_at(f, sh_off, vbuf, vlen)) {
                    for (size_t k = 0; k < vlen && k < sizeof(out.version) - 1; k++) {
                        char c = static_cast<char>(vbuf[k]);
                        if (c == '\0' || c == '\n' || c == '\r') break;
                        out.version[k] = c;
                    }
                }
                log::info("nvidia: fw:   .fwversion    off=0x%lx size=%lu value='%s'",
                          static_cast<unsigned long>(sh_off),
                          static_cast<unsigned long>(sh_size), out.version);
            } else if (str_prefix(name, ".fwsignature")) {
                if (out.fwsig_count < (sizeof(out.fwsig_off) / sizeof(out.fwsig_off[0]))) {
                    out.fwsig_off[out.fwsig_count] = sh_off;
                    out.fwsig_size[out.fwsig_count] = sh_size;
                    out.fwsig_count++;
                }
                // The GSP-RM signature for this chip (Booter verifies against it).
                if (str_prefix(name, ".fwsignature_ga10x")) {
                    out.sig_ga10x_off = sh_off;
                    out.sig_ga10x_size = sh_size;
                }
                log::info("nvidia: fw:   %s off=0x%lx size=%lu",
                          name, static_cast<unsigned long>(sh_off),
                          static_cast<unsigned long>(sh_size));
            }
        }

        if (out.fwimage_off == 0 || out.fwimage_size == 0) {
            log::error("nvidia: fw: .fwimage section not found");
            result = ERR_FW_NOSECTION;
            break;
        }
        if (!str_eq(out.version, GSP_FW_VERSION)) {
            log::error("nvidia: fw: version mismatch: got '%s' want '%s'",
                       out.version, GSP_FW_VERSION);
            result = ERR_FW_VERSION;
            break;
        }

        log::info("nvidia: fw: VALID GSP image - version %s, .fwimage %lu MiB, %u signature(s)",
                  out.version, static_cast<unsigned long>(out.fwimage_size >> 20),
                  out.fwsig_count);
        result = FW_OK;
    } while (0);

    kbuf_free(shdrs);
    kbuf_free(shstr);
    fs::close(f);
    return result;
}

} // namespace nvidia
