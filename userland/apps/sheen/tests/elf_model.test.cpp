/* Host tests for `elf_model`: ELF64 `x86_64` and `aarch64` headers, sections, segments, symbols,
 * DWARF version detection, Hex/ASCII selection spans, caps, and adversarial bounds rejection. */

#include "extensions/perf_binaries/elf_model.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sheen;

struct mutated_u16_rejection {
    size_t offset;
    uint16_t value;
    const char* reason;
};

struct shdr_fixture {
    const char* name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t entsize;
};

struct sym_fixture {
    const char* name;
    uint8_t bind;
    uint8_t type;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

static constexpr size_t EHDR_SIZE = 64, PHDR_SIZE = 56, SHDR_SIZE = 64, SYM_SIZE = 24;
static constexpr uint32_t SHT_PROGBITS_VAL = 1, SHT_SYMTAB_VAL = 2, SHT_STRTAB_VAL = 3;
static constexpr uint32_t SHT_NOBITS_VAL = 8, SHT_SYMTAB_SHNDX_VAL = 18;
static constexpr uint32_t PT_LOAD_VAL = 1, PT_INTERP_VAL = 3, MAX_FAILURE_PRINTS = 12;

static constexpr mutated_u16_rejection BAD_U16_HEADER_CASES[] = {
    { 0x10, 4, "rejects ET_CORE" },
    { 0x12, 40, "rejects EM_ARM" },
    { 0x34, EHDR_SIZE - 1, "rejects e_ehsize < 64" },
    { 0x3e, 8, "rejects shstrndx == shnum" },
    { 0x3e, 1, "rejects non-STRTAB shstrndx" },
    { 0x3c, static_cast<uint16_t>(ELF_MAX_SECTIONS + 1), "rejects shnum > ELF_MAX_SECTIONS" },
    { 0x38, static_cast<uint16_t>(ELF_MAX_PROGRAM_HEADERS + 1), "rejects phnum > ELF_MAX_PROGRAM_HEADERS" },
};

static constexpr sym_fixture FIXTURE_SYMBOLS[] = {
    { "", ELF_STB_LOCAL, ELF_STT_NOTYPE, 0, 0, 0 },
    { "g_frame_budget", ELF_STB_LOCAL, ELF_STT_OBJECT, 2, 0x402000, 8 },
    { "_start", ELF_STB_GLOBAL, ELF_STT_FUNC, 1, 0x401000, 32 },
    { "_ZN6server14compose_damageERK4recti", ELF_STB_GLOBAL, ELF_STT_FUNC, 1, 0x401020, 44 },
    { "weak_hook", ELF_STB_WEAK, ELF_STT_FUNC, 0xfff1u, 0x1000, 0 },
    { "external_ref", ELF_STB_GLOBAL, ELF_STT_NOTYPE, 0, 0, 0 },
};

static uint64_t g_checks = 0;
static uint64_t g_failures = 0;

static void check(bool holds, const char* description) {
    g_checks++;
    if (!holds && ++g_failures <= MAX_FAILURE_PRINTS) {
        std::printf("  FAIL: %s\n", description);
    }
}

static void put_u16(std::vector<uint8_t>& buf, size_t off, uint16_t val) {
    buf[off] = static_cast<uint8_t>(val & 0xffu);
    buf[off + 1] = static_cast<uint8_t>((val >> 8) & 0xffu);
}

static void put_u32(std::vector<uint8_t>& buf, size_t off, uint32_t val) {
    put_u16(buf, off, static_cast<uint16_t>(val & 0xffffu));
    put_u16(buf, off + 2, static_cast<uint16_t>((val >> 16) & 0xffffu));
}

static void put_u64(std::vector<uint8_t>& buf, size_t off, uint64_t val) {
    put_u32(buf, off, static_cast<uint32_t>(val & 0xffffffffull));
    put_u32(buf, off + 4, static_cast<uint32_t>((val >> 32) & 0xffffffffull));
}

static uint32_t append_str(std::vector<uint8_t>& strtab, const char* text) {
    uint32_t off = static_cast<uint32_t>(strtab.size());
    strtab.insert(strtab.end(), reinterpret_cast<const uint8_t*>(text),
                  reinterpret_cast<const uint8_t*>(text) + std::strlen(text) + 1);
    return off;
}

static void write_shdr(std::vector<uint8_t>& buf, size_t off, uint32_t name, const shdr_fixture& s, uint64_t foff) {
    put_u32(buf, off + 0x00, name);
    put_u32(buf, off + 0x04, s.type);
    put_u64(buf, off + 0x08, s.flags);
    put_u64(buf, off + 0x10, s.addr);
    put_u64(buf, off + 0x18, foff);

    put_u64(buf, off + 0x20, s.size);
    put_u32(buf, off + 0x28, s.link);
    put_u32(buf, off + 0x2c, s.info);
    put_u64(buf, off + 0x30, 8);
    put_u64(buf, off + 0x38, s.entsize);
}

static std::vector<uint8_t> make_valid_x86_64_exec() {
    std::vector<uint8_t> strtab;
    std::vector<uint8_t> symtab;
    for (const sym_fixture& sym : FIXTURE_SYMBOLS) {
        uint32_t st_name = append_str(strtab, sym.name);
        size_t base = symtab.size();
        symtab.resize(base + SYM_SIZE, 0);
        put_u32(symtab, base, st_name);

        symtab[base + 4] = static_cast<uint8_t>((sym.bind << 4) | (sym.type & 0x0fu));
        put_u16(symtab, base + 6, sym.shndx);
        put_u64(symtab, base + 8, sym.value);
        put_u64(symtab, base + 16, sym.size);
    }

    std::vector<uint8_t> shstrtab;
    const shdr_fixture sections[8] = {
        { "", 0, 0, 0, 0, 0, 0, 0 },
        { ".text", SHT_PROGBITS_VAL, ELF_SHF_ALLOC | ELF_SHF_EXECINSTR, 0x401000, 64, 0, 0, 0 },
        { ".rodata", SHT_PROGBITS_VAL, ELF_SHF_ALLOC, 0x402000, 32, 0, 0, 0 },
        { ".bss", SHT_NOBITS_VAL, ELF_SHF_ALLOC | ELF_SHF_WRITE, 0x402020, 128, 0, 0, 0 },
        { ".debug_info", SHT_PROGBITS_VAL, 0, 0, 12, 0, 0, 0 },
        { ".symtab", SHT_SYMTAB_VAL, 0, 0, symtab.size(), 6, 2, SYM_SIZE },
        { ".strtab", SHT_STRTAB_VAL, 0, 0, strtab.size(), 0, 0, 0 },
        { ".shstrtab", SHT_STRTAB_VAL, 0, 0, 0, 0, 0, 0 },
    };

    uint32_t sh_names[8] = {};
    for (size_t i = 0; i < 8; i++) {
        sh_names[i] = append_str(shstrtab, sections[i].name);
    }

    size_t phoff = EHDR_SIZE;
    size_t text_off = phoff + 2 * PHDR_SIZE;
    size_t rodata_off = text_off + 64;
    size_t debug_off = rodata_off + 32;

    size_t symtab_off = debug_off + 12;
    size_t strtab_off = symtab_off + symtab.size();
    size_t shstrtab_off = strtab_off + strtab.size();
    size_t shoff = shstrtab_off + shstrtab.size();

    std::vector<uint8_t> image(shoff + 8 * SHDR_SIZE, 0);
    std::memcpy(image.data(), "\x7f" "ELF\x02\x01\x01", 7);
    put_u16(image, 0x10, ELF_TYPE_EXEC);
    put_u16(image, 0x12, ELF_MACHINE_X86_64);
    put_u32(image, 0x14, 1);
    put_u64(image, 0x18, 0x401080);

    put_u64(image, 0x20, phoff);
    put_u64(image, 0x28, shoff);
    put_u16(image, 0x34, EHDR_SIZE);
    put_u16(image, 0x36, PHDR_SIZE);
    put_u16(image, 0x38, 2);

    put_u16(image, 0x3a, SHDR_SIZE);
    put_u16(image, 0x3c, 8);
    put_u16(image, 0x3e, 7);

    std::memset(image.data() + text_off, 0x90, 64);
    std::memcpy(image.data() + rodata_off, "StelluxOS", 9);
    put_u32(image, debug_off, 8);
    put_u16(image, debug_off + 4, 5);

    std::memcpy(image.data() + symtab_off, symtab.data(), symtab.size());
    std::memcpy(image.data() + strtab_off, strtab.data(), strtab.size());
    std::memcpy(image.data() + shstrtab_off, shstrtab.data(), shstrtab.size());

    auto write_ph = [&](size_t off, uint32_t flg, uint64_t foff, uint64_t va, uint64_t fsz, uint64_t msz) {
        put_u32(image, off, PT_LOAD_VAL);
        put_u32(image, off + 4, flg);
        put_u64(image, off + 8, foff);
        put_u64(image, off + 16, va);

        put_u64(image, off + 24, va);
        put_u64(image, off + 32, fsz);
        put_u64(image, off + 40, msz);
        put_u64(image, off + 48, 0x1000);
    };

    write_ph(phoff, ELF_PF_R | ELF_PF_X, text_off, 0x401000, 64, 64);
    write_ph(phoff + PHDR_SIZE, ELF_PF_R | ELF_PF_W, rodata_off, 0x402000, 32, 160);

    const uint64_t foffs[8] = { 0, text_off, rodata_off, rodata_off + 32, debug_off, symtab_off, strtab_off, shstrtab_off };
    for (size_t i = 0; i < 8; i++) {
        shdr_fixture spec = sections[i];
        if (i == 7) {
            spec.size = shstrtab.size();
        }

        write_shdr(image, shoff + i * SHDR_SIZE, sh_names[i], spec, foffs[i]);
    }

    return image;
}

static bool parses(const std::vector<uint8_t>& image) {
    elf_binary_model model;
    std::string error;
    return parse_elf_binary(image, model, error);
}

int main() {
    const std::vector<uint8_t> valid = make_valid_x86_64_exec();
    elf_binary_model model;
    std::string error;
    check(is_elf64_binary(valid) && parse_elf_binary(valid, model, error), "parses valid x86_64 ET_EXEC binary");
    check(model.header.type_name == "ET_EXEC" && model.header.arch_name == "x86_64" &&
              model.header.is_static && model.header.dwarf_version == 5,
          "extracts type, arch, static linkage, and DWARF v5");

    if (model.sections.size() == 8 && model.symbols.size() == 5) {
        byte_range sym_range;
        check(model.sections[1].type_name == "PROGBITS" && model.sections[1].flags_label == "R X" &&
                  model.sections[3].is_nobits && model.sections[3].share == 0.0 &&
                  model.sections[5].type_name == "SYMTAB" && model.sections[5].flags_label.empty(),
              "separates section type_name from flags_label and excludes .bss from file share");
        check(model.symbols[2].demangled_name == "server::compose_damage(rect const&, int)" &&
                  symbol_file_byte_range(model, model.symbols[2], sym_range) && sym_range.end > sym_range.start,
              "demangles C++ symbol and resolves file byte range");
    }

    std::string huge_mangled = "_Z" + std::string(ELF_MAX_DEMANGLE_INPUT + 10, 'i');
    check(demangle_elf_symbol(huge_mangled) == huge_mangled, "skips demangling past ELF_MAX_DEMANGLE_INPUT");

    size_t shoff = static_cast<size_t>(valid[0x28]) | (static_cast<size_t>(valid[0x29]) << 8);
    std::vector<uint8_t> dup_symtab = valid;
    dup_symtab.insert(dup_symtab.end(), dup_symtab.begin() + static_cast<ptrdiff_t>(shoff + 5 * SHDR_SIZE),
                      dup_symtab.begin() + static_cast<ptrdiff_t>(shoff + 6 * SHDR_SIZE));
    put_u16(dup_symtab, 0x3c, 9);
    check(parse_elf_binary(dup_symtab, model, error) && model.symbols.size() == 5, "deduplicates SHT_SYMTAB ranges");

    std::vector<uint8_t> dyn_bytes = valid;
    put_u16(dyn_bytes, 0x10, ELF_TYPE_DYN);
    put_u16(dyn_bytes, 0x12, ELF_MACHINE_AARCH64);
    put_u32(dyn_bytes, EHDR_SIZE + PHDR_SIZE, PT_INTERP_VAL);
    put_u64(dyn_bytes, EHDR_SIZE + PHDR_SIZE + 0x28, 32);
    check(parse_elf_binary(dyn_bytes, model, error) && !model.header.is_static && model.header.arch_name == "aarch64",
          "parses dynamic aarch64 ET_DYN");

    size_t symtab_off = static_cast<size_t>(valid[shoff + 5 * SHDR_SIZE + 0x18]) |
                        (static_cast<size_t>(valid[shoff + 5 * SHDR_SIZE + 0x19]) << 8);
    std::vector<uint8_t> xndx = valid;
    put_u16(xndx, symtab_off + 2 * SYM_SIZE + 0x06, 0xffffu);
    check(!parses(xndx), "rejects SHN_XINDEX symbol without SHT_SYMTAB_SHNDX");

    std::vector<uint8_t> shndx_payload(24, 0);
    put_u32(shndx_payload, 8, 1);
    size_t shndx_off = xndx.size();
    xndx.insert(xndx.end(), shndx_payload.begin(), shndx_payload.end());

    size_t new_shoff = xndx.size();
    xndx.insert(xndx.end(), xndx.begin() + static_cast<ptrdiff_t>(shoff),
                xndx.begin() + static_cast<ptrdiff_t>(shoff + 8 * SHDR_SIZE));
    xndx.resize(xndx.size() + SHDR_SIZE, 0);

    write_shdr(xndx, new_shoff + 8 * SHDR_SIZE, 0, { "", SHT_SYMTAB_SHNDX_VAL, 0, 0, 24, 5, 0, 4 }, shndx_off);
    put_u64(xndx, 0x28, new_shoff);
    put_u16(xndx, 0x3c, 9);
    check(parse_elf_binary(xndx, model, error) && model.symbols[1].section_name == ".text",
          "resolves SHN_XINDEX symbol through SHT_SYMTAB_SHNDX");

    hex_ascii_window win = format_hex_ascii_window(valid, 0, 2, elf_header_field_range("Ehdr.e_entry"));
    check(win.rows.size() == 2 && !win.rows[0].has_selection && win.rows[1].has_selection &&
              win.rows[1].line.substr(win.rows[1].hex_column_start,
                                      win.rows[1].hex_column_end - win.rows[1].hex_column_start) ==
                  "80 10 40 00 00 00 00 00",
          "highlights exact Ehdr.e_entry hex bytes on row 0x10");

    std::vector<uint8_t> arm32_hdr = valid;
    put_u16(arm32_hdr, 0x12, 40);
    check(!is_elf64_binary(std::span<const uint8_t>(arm32_hdr.data(), 24)) &&
              !parses({}) && !parses(std::vector<uint8_t>(valid.begin(), valid.begin() + 63)),
          "rejects unsupported machine prefix and truncated buffers");

    for (const mutated_u16_rejection& test_case : BAD_U16_HEADER_CASES) {
        std::vector<uint8_t> bad = valid;
        put_u16(bad, test_case.offset, test_case.value);
        check(!parses(bad), test_case.reason);
    }

    uint64_t shstr_off = static_cast<uint64_t>(valid[shoff + 7 * SHDR_SIZE + 0x18]) |
                         (static_cast<uint64_t>(valid[shoff + 7 * SHDR_SIZE + 0x19]) << 8);
    uint32_t shstr_sz = static_cast<uint32_t>(valid[shoff + 7 * SHDR_SIZE + 0x20]);

    auto mutate = [&](auto fn, const char* msg) {
        std::vector<uint8_t> bad = valid;
        fn(bad);
        check(!parses(bad), msg);
    };

    mutate([](auto& b) { b[4] = 1; }, "rejects ELFCLASS32");
    mutate([](auto& b) { put_u16(b, 0x38, 1); put_u16(b, 0x36, PHDR_SIZE - 1); }, "rejects phentsize == 55");
    mutate([](auto& b) { put_u64(b, 0x20, UINT64_MAX - 16); }, "rejects Phdr phoff + size overflow");
    mutate([](auto& b) { put_u64(b, EHDR_SIZE + 0x20, 64); put_u64(b, EHDR_SIZE + 0x28, 63); }, "rejects filesz > memsz");

    mutate([&](auto& b) { put_u16(b, 0x3c, 0); put_u16(b, 0x3e, 0); put_u64(b, shoff + 0x20, 0x0400000000000001ull); },
           "rejects extended shnum 64-bit product overflow");
    mutate([&](auto& b) { put_u16(b, symtab_off + SYM_SIZE + 0x06, 8); }, "rejects symbol st_shndx == shnum");
    mutate([&](auto& b) { put_u64(b, symtab_off + SYM_SIZE + 0x08, UINT64_MAX - 4); put_u64(b, symtab_off + SYM_SIZE + 0x10, 8); },
           "rejects symbol st_value + st_size overflow");

    mutate([&](auto& b) {
        for (size_t i = 0; i < shstr_sz; i++) {
            if (b[static_cast<size_t>(shstr_off) + i] == '\0') {
                b[static_cast<size_t>(shstr_off) + i] = 'X';
            }
        }
    }, "rejects unterminated string table");

    std::vector<uint8_t> bad_utf8 = valid;
    bad_utf8[static_cast<size_t>(shstr_off) + 1] = 0xffu;
    elf_binary_model utf8_model;
    check(parse_elf_binary(bad_utf8, utf8_model, error) && utf8_model.sections[1].name == "?text",
          "sanitizes invalid UTF-8 bytes in string table labels");

    if (g_failures != 0) {
        std::printf("elf_model: %llu of %llu checks failed\n",
                    static_cast<unsigned long long>(g_failures),
                    static_cast<unsigned long long>(g_checks));
        return 1;
    }

    std::printf("elf_model: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
