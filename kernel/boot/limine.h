/*
 * Limine Boot Protocol Header
 * 
 * This is the kernel-side header for the Limine boot protocol.
 * See: https://github.com/limine-bootloader/limine/blob/trunk/PROTOCOL.md
 * 
 * Pinned to Limine v8+ protocol (works with v10.x bootloader)
 */

#ifndef _LIMINE_H
#define _LIMINE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Limine uses these macros to handle pointer sizes across architectures */
#ifdef __x86_64__
#define LIMINE_PTR(TYPE) TYPE
#elif defined(__aarch64__)
#define LIMINE_PTR(TYPE) TYPE
#else
#error "Unsupported architecture"
#endif

/* Request ID magic numbers */
#define LIMINE_COMMON_MAGIC_0 0xc7b1dd30df4c8b88
#define LIMINE_COMMON_MAGIC_1 0x0a82e883a194f07b

/*
 * Framebuffer Request
 * Requests linear framebuffer(s) from the bootloader
 */

#define LIMINE_FRAMEBUFFER_REQUEST_ID_0 0x9d5827dcd881dd75
#define LIMINE_FRAMEBUFFER_REQUEST_ID_1 0xa3148604f6fab11b

#define LIMINE_FRAMEBUFFER_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_FRAMEBUFFER_REQUEST_ID_0, LIMINE_FRAMEBUFFER_REQUEST_ID_1 }

/* Memory models */
#define LIMINE_FRAMEBUFFER_RGB 1

struct limine_video_mode {
    uint64_t pitch;
    uint64_t width;
    uint64_t height;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

struct limine_framebuffer {
    LIMINE_PTR(void *) address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    uint8_t unused[7];
    uint64_t edid_size;
    LIMINE_PTR(void *) edid;
    /* Revision 1+ */
    uint64_t mode_count;
    LIMINE_PTR(struct limine_video_mode **) modes;
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    LIMINE_PTR(struct limine_framebuffer **) framebuffers;
};

struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_framebuffer_response *) response;
};

/*
 * HHDM (Higher Half Direct Map) Request
 * Provides a virtual address offset for accessing physical memory
 */

#define LIMINE_HHDM_REQUEST_ID_0 0x48dcf1cb8ad2b852
#define LIMINE_HHDM_REQUEST_ID_1 0x63984e959a98244b

#define LIMINE_HHDM_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_HHDM_REQUEST_ID_0, LIMINE_HHDM_REQUEST_ID_1 }

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_hhdm_response *) response;
};

/*
 * Entry Point Request (Required for Limine protocol)
 * Tells Limine where to jump after setup
 */

#define LIMINE_ENTRY_POINT_REQUEST_ID_0 0x13d86c035a1cd3e1
#define LIMINE_ENTRY_POINT_REQUEST_ID_1 0x2b0caa89d8f3026a

#define LIMINE_ENTRY_POINT_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_ENTRY_POINT_REQUEST_ID_0, LIMINE_ENTRY_POINT_REQUEST_ID_1 }

typedef void (*limine_entry_point)(void);

struct limine_entry_point_response {
    uint64_t revision;
};

struct limine_entry_point_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_entry_point_response *) response;
    LIMINE_PTR(limine_entry_point) entry;
};

/*
 * Kernel Address Request
 * Provides the virtual and physical base addresses of the kernel
 */

#define LIMINE_KERNEL_ADDRESS_REQUEST_ID_0 0x71ba76863cc55f63
#define LIMINE_KERNEL_ADDRESS_REQUEST_ID_1 0xb2644a48c516a487

#define LIMINE_KERNEL_ADDRESS_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_KERNEL_ADDRESS_REQUEST_ID_0, LIMINE_KERNEL_ADDRESS_REQUEST_ID_1 }

struct limine_kernel_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct limine_kernel_address_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_kernel_address_response *) response;
};

/*
 * Memory Map Request
 * Provides the system memory map with region types
 */

#define LIMINE_MEMMAP_REQUEST_ID_0 0x67cf3d9d378a806f
#define LIMINE_MEMMAP_REQUEST_ID_1 0xe304acdfc50c3c62

#define LIMINE_MEMMAP_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_MEMMAP_REQUEST_ID_0, LIMINE_MEMMAP_REQUEST_ID_1 }

/* Memory region types */
#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    LIMINE_PTR(struct limine_memmap_entry **) entries;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_memmap_response *) response;
};

/*
 * RSDP Request
 * Provides the ACPI Root System Description Pointer
 */

#define LIMINE_RSDP_REQUEST_ID_0 0xc5e77b6b397e7b43
#define LIMINE_RSDP_REQUEST_ID_1 0x27637845accdcf3c

#define LIMINE_RSDP_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_RSDP_REQUEST_ID_0, LIMINE_RSDP_REQUEST_ID_1 }

struct limine_rsdp_response {
    uint64_t revision;
    LIMINE_PTR(void *) address; /* Physical address of RSDP */
};

struct limine_rsdp_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_rsdp_response *) response;
};

/*
 * DTB Request
 * Provides the Device Tree Blob (primarily for AArch64)
 */

#define LIMINE_DTB_REQUEST_ID_0 0xb40ddb48fb54bac7
#define LIMINE_DTB_REQUEST_ID_1 0x545081493f81ffb7

#define LIMINE_DTB_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_DTB_REQUEST_ID_0, LIMINE_DTB_REQUEST_ID_1 }

struct limine_dtb_response {
    uint64_t revision;
    LIMINE_PTR(void *) dtb_ptr; /* Virtual address of DTB */
};

struct limine_dtb_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_dtb_response *) response;
};

/*
 * Kernel File Request
 * Provides information about the loaded kernel file
 */

#define LIMINE_KERNEL_FILE_REQUEST_ID_0 0xad97e90e83f1ed67
#define LIMINE_KERNEL_FILE_REQUEST_ID_1 0x31eb5d1c5ff23b69

#define LIMINE_KERNEL_FILE_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_KERNEL_FILE_REQUEST_ID_0, LIMINE_KERNEL_FILE_REQUEST_ID_1 }

struct limine_uuid {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t d[8];
};

struct limine_file {
    uint64_t revision;
    LIMINE_PTR(void *) address;        /* Virtual address of file */
    uint64_t size;
    LIMINE_PTR(char *) path;
    LIMINE_PTR(char *) cmdline;
    uint32_t media_type;
    uint32_t unused;
    uint32_t tftp_ip;
    uint32_t tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    struct limine_uuid gpt_disk_uuid;
    struct limine_uuid gpt_part_uuid;
    struct limine_uuid part_uuid;
};

struct limine_kernel_file_response {
    uint64_t revision;
    LIMINE_PTR(struct limine_file *) kernel_file;
};

struct limine_kernel_file_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_kernel_file_response *) response;
};

/*
 * Module Request
 * Provides access to loaded modules (initrd, etc.)
 */

#define LIMINE_MODULE_REQUEST_ID_0 0x3e7e279702be32af
#define LIMINE_MODULE_REQUEST_ID_1 0xca1c4f3bd1280cee

#define LIMINE_MODULE_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      LIMINE_MODULE_REQUEST_ID_0, LIMINE_MODULE_REQUEST_ID_1 }

/* Internal module request flags */
#define LIMINE_INTERNAL_MODULE_REQUIRED   (1 << 0)
#define LIMINE_INTERNAL_MODULE_COMPRESSED (1 << 1)

struct limine_internal_module {
    LIMINE_PTR(const char *) path;
    LIMINE_PTR(const char *) cmdline;
    uint64_t flags;
};

struct limine_module_response {
    uint64_t revision;
    uint64_t module_count;
    LIMINE_PTR(struct limine_file **) modules;
};

struct limine_module_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_module_response *) response;
    uint64_t internal_module_count;
    LIMINE_PTR(struct limine_internal_module **) internal_modules;
};

/*
 * Base Revision
 * Declares the minimum Limine protocol revision the kernel supports
 */

#define LIMINE_BASE_REVISION_0 0xf9562b2d5c95a6c8
#define LIMINE_BASE_REVISION_1 0x6a7b384944536bdc

#define LIMINE_BASE_REVISION(N) \
    uint64_t limine_base_revision[3] = { LIMINE_BASE_REVISION_0, LIMINE_BASE_REVISION_1, (N) };

/*
 * Helper macro to check if a request was fulfilled
 */
#ifdef __cplusplus
#define LIMINE_REQUEST_FULFILLED(req) ((req).response != nullptr)
#else
#define LIMINE_REQUEST_FULFILLED(req) ((req).response != ((void*)0))
#endif

#ifdef __cplusplus
}
#endif

#endif /* _LIMINE_H */
