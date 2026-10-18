// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_FILES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_FILES_H

#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/registers.h"
#include "fw/fw.h"

namespace drivers::nvidia {

// The firmware version the driver boots on every chip. It names the directory under nvidia/ in `fw::DIRECTORY`
// that holds its files, and the GSP image's .fwversion section must hold it.
constexpr const char FIRMWARE_VERSION[] = "535.183.01";

// The firmware files bring-up opens, each open until the object is destroyed when `attach()` returns
struct firmware_files {
    fw::file bootloader;
    fw::file booter_load;
    fw::file booter_unload;
    fw::file gsp_image;
};

/**
 * The header at the start of a bootloader or booter file, six little-endian
 * 32-bit fields, as NVIDIA's extraction tool writes it: the script in NVIDIA's
 * open kernel modules that writes those programs out as files. Between the
 * header and the program lie the descriptors, records that say where the
 * program's parts and signatures lie, and the signatures each kind of file
 * carries.
 */
struct boot_file_header {
    uint32_t magic;             // 0x10DE
    uint32_t header_version;    // 1
    uint32_t rounded_size;      // the file's length rounded up to a multiple of 256 bytes, so it can exceed that length
    uint32_t descriptor_offset; // where the descriptors start, right after this header
    uint32_t program_offset;    // where the program starts, at or after the descriptors
    uint32_t program_size;      // bytes
};

static_assert(sizeof(boot_file_header) == 24, "boot_file_header must be 24 bytes");

/**
 * Where three parts of the bootloader's program start, and the program's app
 * version, as the bootloader's descriptor records them. That descriptor, the
 * bytes between the bootloader's header and its program, is NVIDIA's
 * descriptor of a program for the GSP's RISC-V core. The parts are the
 * monitor's code, the monitor's data and the manifest. The monitor is a part
 * of the program the core runs, and the manifest describes it to the core's
 * boot ROM, the code the core runs from its own ROM as it starts. Each part
 * lies inside the program.
 */
struct bootloader_layout {
    uint32_t manifest_offset;     // bytes from the start of the program
    uint32_t monitor_data_offset; // bytes from the start of the program
    uint32_t monitor_code_offset; // bytes from the start of the program
    uint32_t app_version;         // NVIDIA's change number for the program's build, which may be 0
};

/**
 * Where a booter's code, data, signature slot and signatures lie, with its
 * ucode id and engine mask, as its descriptors record them. A booter is
 * either of two files holding a program that SEC2, the GPU's security
 * processor, runs on its falcon, the microcontroller in SEC2. Booter load's
 * program loads the GSP's firmware into protected memory (the video memory
 * reserved for that firmware), verifies it and boots it, and booter unload's
 * program takes protected memory down again. The falcon's boot ROM, the code
 * the falcon runs from its own ROM as it starts, runs the code only once the
 * signature in the signature slot checks out. A booter carries signatures for
 * one or more of the versions the chip's fuses, bits burned into the chip for
 * good, may record for it, and `select_booter_signature` chooses the one the
 * slot must receive. The code and data each lie inside the program, the slot
 * lies inside the data, and the signatures lie one after another inside the
 * file.
 */
struct booter_layout {
    uint32_t code_offset;       // bytes from the start of the program
    uint32_t code_size;         // bytes
    uint32_t data_offset;       // bytes from the start of the program
    uint32_t data_size;         // bytes
    uint32_t signature_slot;    // bytes from the start of the data
    uint32_t signatures_offset; // bytes from the start of the file
    uint32_t signature_size;    // bytes of each signature, at least 1
    uint32_t signature_count;   // at least 1
    uint32_t ucode_id;          // selects SEC2's fuse register for the booter's version and is written to its boot ROM
    uint32_t engine_mask;       // the engines the booter is signed to run on
};

// Which of a booter's signatures its signature slot must hold, and the fuse version read to choose it
struct booter_signature {
    uint32_t fuse_version; // the version `read_ucode_fuse_version` gives for the booter, from 0 to 32
    uint32_t index;        // the signature's place among the booter's signatures, counting from 0
};

/**
 * Where the GSP image places its firmware and the chip's signature for it,
 * each a section holding at least one byte inside the file. The firmware is
 * the .fwimage section, which booter load's program loads into protected
 * memory, verifies with the signature and boots. The signature is the section
 * the chip table names for the chip, since the image holds signatures for
 * several kinds of chip.
 */
struct gsp_image_layout {
    uint64_t firmware_file_offset;  // bytes from the start of the file
    uint64_t firmware_size;         // bytes, at least 1
    uint64_t signature_file_offset; // bytes from the start of the file
    uint64_t signature_size;        // bytes, at least 1
};

/**
 * @brief Open a chip's firmware files, in the layout the nvidia-firmware
 * package installs under `fw::DIRECTORY`: the bootloader, booter load and
 * booter unload in the chip's boot directory in nvidia/<FIRMWARE_VERSION>/
 * first, then the GSP image in nvidia/<FIRMWARE_VERSION>/, logging each
 * file's name and size. `files_out` must hold no file open, and keeps every
 * file that opened until it is destroyed, whatever the result.
 * @return `OK`, `DECLINED` when a directory the files belong in is not
 * installed, or `ERR_BAD_FIRMWARE` when a file of an installed directory
 * cannot be opened, a missing file included, or when a name built from
 * `firmware` is too long or malformed. The cause is logged.
 */
int32_t open_firmware_files(const chip_firmware& firmware, firmware_files& files_out);

/**
 * @brief Translate a `fw::file` result into what it means for bring-up.
 * @return `OK` for `fw::OK`, `DECLINED` for `fw::ERR_NOT_INSTALLED`, and
 * `ERR_BAD_FIRMWARE` for every other result.
 */
int32_t to_bring_up_result(int32_t fw_result);

/**
 * @brief Check that the GSP image is a 64-bit little-endian ELF file whose
 * single .fwversion section holds exactly `FIRMWARE_VERSION` and its NUL, and
 * find where it places its firmware and the chip's signature. The .fwversion,
 * .fwimage and signature sections must each be the only section of its name,
 * hold at least one byte and lie inside the file. Reads the ELF header, the
 * section headers, for each name sought the bytes at the start of each
 * section's name that the name and its NUL take when they lie inside the name
 * table, and the .fwversion section, each with `fw::file::read`, which takes
 * the file's mutex, so this must not run in an interrupt handler or under a
 * spinlock. On the boot thread, which cannot wait, `image` must be one no
 * other thread uses at the time, as `fw::file` describes.
 * @param image Must hold a file open.
 * @param signature_section The name of the section holding the chip's
 * signature, such as the chip table's `gsp_signature_section`, which must not
 * be null.
 * @param layout_out Receives where the firmware and the signature lie on
 * `OK`, each then holding at least one byte inside the file, and is left
 * unchanged on an error.
 * @return `OK`, or `ERR_BAD_FIRMWARE` when the file is shorter than an ELF
 * header or not a 64-bit little-endian ELF file, its section headers are not
 * 64 bytes each, it has no section or more than 64, the most section headers
 * it reads, its section table or section name table lies outside the file,
 * the name table's index is 0 or past the last section, it has no .fwversion,
 * .fwimage or signature section or more than one of a name, one of them holds
 * no byte or lies outside the file, the .fwversion section is larger than the
 * 32 bytes it reads a version into or holds anything but `FIRMWARE_VERSION`
 * and its NUL, `signature_section` is longer than the 31 bytes of a name the
 * check compares, or reading fails. The cause is logged.
 */
int32_t check_gsp_image(const fw::file& image, const char* signature_section, gsp_image_layout& layout_out);

/**
 * @brief Check that a bootloader or booter starts with a `boot_file_header`
 * whose descriptors start right after it and whose program starts at or after
 * the descriptors and ends inside the file. The rounded size, which can exceed
 * the file's length, is not compared with it. The header is read with
 * `fw::file::read`, which takes the file's mutex, so this must not run in an
 * interrupt handler or under a spinlock. On the boot thread, which cannot
 * wait, `file` must be one no other thread uses at the time, as `fw::file`
 * describes.
 * @param file Must hold a file open.
 * @param name What the file is, such as "bootloader", for the log line.
 * @param header_out Receives the header on `OK`, whose descriptors then start
 * right after it and whose program starts at or after them and ends inside
 * the file, and is left unchanged on an error.
 * @return `OK`, or `ERR_BAD_FIRMWARE` when the file is shorter than the
 * header, the magic is not 0x10DE or the header version not 1, the
 * descriptors do not start right after the header, the program starts before
 * them or ends past the file, or reading fails. The cause is logged, naming
 * `name`.
 */
int32_t check_boot_file(const fw::file& file, const char* name, boot_file_header& header_out);

/**
 * @brief Read the bootloader's layout from its descriptor and check that the
 * descriptor holds every field read and places the manifest and the
 * monitor's data and code each inside the program. The descriptor is read
 * with `fw::file::read`, which takes the file's mutex, so this must not run
 * in an interrupt handler or under a spinlock. On the boot thread, which
 * cannot wait, `bootloader` must be one no other thread uses at the time, as
 * `fw::file` describes.
 * @param bootloader Must hold the bootloader open.
 * @param header Must be the header `check_boot_file` returned for
 * `bootloader`, whose program then starts at or after its descriptor.
 * @param layout_out Receives the layout on `OK`, each of whose parts then ends
 * inside the program, and is left unchanged on an error.
 * @return `OK`, or `ERR_BAD_FIRMWARE` when the descriptor, from the end of
 * the header to the start of the program, is shorter than the 56 bytes up to
 * the end of the last field read, when the manifest or the monitor's data or
 * code ends past the program, or when reading fails. The cause is logged.
 */
int32_t read_bootloader_layout(
    const fw::file& bootloader,
    const boot_file_header& header,
    bootloader_layout& layout_out
);

/**
 * @brief Read a booter's layout from its descriptors and check it. Booter
 * load and booter unload follow the same rules. The descriptors start with a
 * header of nine 32-bit fields, each a size or an offset from the start of
 * the file, that lead to the rest: the signatures, the slot's offset from
 * the start of the program, NVIDIA's signature patch index, which NVIDIA's
 * driver requires to be 0 and otherwise ignores, the metadata, which ends
 * with the booter's engine mask and ucode id, the signature count, and the
 * load header. The load header places the data and the program's apps,
 * NVIDIA's name for the parts of a falcon program's code its boot ROM checks
 * against a signature. A booter has one app, whose code is the code. Each
 * field is read with `fw::file::read`, which takes the file's mutex, so this
 * must not run in an interrupt handler or under a spinlock. On the boot
 * thread, which cannot wait, `booter` must be one no other thread uses at
 * the time, as `fw::file` describes.
 * @param booter Must hold a booter open.
 * @param name Which booter it is, such as "booter load", for the log lines.
 * @param header Must be the header `check_boot_file` returned for `booter`,
 * whose program then starts at or after its descriptors.
 * @param layout_out Receives the layout on `OK`, whose parts then lie as
 * `booter_layout` describes and whose slot ends before the program's end,
 * and is left unchanged on an error.
 * @return `OK`, or `ERR_BAD_FIRMWARE` when the descriptors, from the end of
 * the header to the start of the program, are shorter than their 36-byte
 * header, the metadata is not 12 bytes or the load header not 36, a
 * descriptor, the metadata or the load header ends past the file, the
 * signature patch index is not 0, the count is 0, the signatures take 0
 * bytes, a number not a multiple of the count, or end past the file, the
 * load header lists other than one app, the code or data ends past the
 * program, the slot does not lie inside the data or does not end before the
 * program's end, or when reading fails. The cause is logged, naming `name`.
 */
int32_t read_booter_layout(
    const fw::file& booter,
    const char* name,
    const boot_file_header& header,
    booter_layout& layout_out
);

/**
 * @brief Choose which of a booter's signatures its signature slot must hold.
 * As in NVIDIA's driver, a lone signature is chosen whatever version the
 * chip's fuses record, and with more than one the one for fuse version v is
 * at index `signature_count` - 1 - v, where `read_ucode_fuse_version` gives
 * v for SEC2's falcon and the booter's ucode id.
 * @param regs A view of BAR0.
 * @param layout The booter's layout, which must be as `read_booter_layout`
 * returns it, so it holds at least one signature.
 * @param name Which booter it is, such as "booter load", for the log line.
 * @param signature_out Receives the choice on `OK`, whose `index` is then
 * below `layout.signature_count`, so the signature lies inside the file, and
 * is left unchanged on an error.
 * @return `OK`, or `ERR_BAD_FIRMWARE` when the booter has more than one
 * signature and the fuse version is above `signature_count` - 1, so none is
 * for it. The cause is logged, naming `name`.
 */
int32_t select_booter_signature(
    const registers& regs,
    const booter_layout& layout,
    const char* name,
    booter_signature& signature_out
);

/**
 * @brief Describe a copy of a booter's program as the program SEC2's falcon
 * runs, as NVIDIA's driver loads booter load and booter unload: the code from
 * its offset into IMEM from IMEM's start, its blocks tagged with virtual
 * addresses from the code's offset, and the data from its own offset into DMEM
 * from DMEM's start, with the signature slot as the signature's offset in the
 * data and the engine mask whole. The ucode id keeps its low 8 bits, the field
 * of the boot ROM's register NVIDIA's driver writes it into. Neither offset's
 * alignment is checked, as in NVIDIA's driver, so a copy on a 256-byte
 * boundary meets `run_falcon_program`'s alignment only when the data's offset
 * is a multiple of 256, and holds each part's whole blocks when the code's
 * offset is one as well.
 * @param layout The booter's layout, as `read_booter_layout` returns it.
 * @param dma_address Where the copy starts, as the card addresses memory. The
 * copy must hold the booter's program, uncached, as
 * `gsp_boot_memory::copy_booter_load` copies booter load and
 * `copy_booter_unload` copies booter unload.
 * @param reads_cached_memory Whether the CPU maps cached the memory the booter
 * reads once started. Booter load reads protected memory's metadata and the
 * parts of the GSP's boot memory the metadata lists, which `gsp_boot_memory`
 * maps cached for a coherent card, and booter unload's mailboxes carry no
 * address.
 * @return The program `run_falcon_program` loads from the copy.
 */
falcon_program describe_booter_program(
    const booter_layout& layout,
    uint64_t dma_address,
    bool reads_cached_memory
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_FILES_H
