#include "drivers/gpu/nvidia/nv_bootimg.h"
#include "fs/fs.h"
#include "common/logging.h"

namespace nvidia {

static int32_t read_whole(const char* path, void* dst, size_t expect) {
    int32_t oerr = 0;
    fs::file* f = fs::open(path, fs::O_RDONLY, &oerr);
    if (!f) {
        log::error("nvidia: bootimg: open(%s) failed err=%d", path, oerr);
        return ERR_BOOTIMG_IO;
    }
    const ssize_t got = fs::read(f, dst, expect);
    fs::close(f);
    if (got != static_cast<ssize_t>(expect)) {
        log::error("nvidia: bootimg: read(%s) got=%ld expect=%lu",
                   path, static_cast<long>(got), static_cast<unsigned long>(expect));
        return ERR_BOOTIMG_SIZE;
    }
    return BOOTIMG_OK;
}

int32_t gsp_load_boot_ucode(gsp_boot_ucode& out) {
    out.image = dma_buffer{};
    out.image_size = 0;
    for (size_t i = 0; i < sizeof(out.desc) / sizeof(uint32_t); i++) {
        reinterpret_cast<uint32_t*>(&out.desc)[i] = 0;
    }

    // 1) Descriptor (RM_RISCV_UCODE_DESC, 84 bytes).
    fs::vattr dattr;
    if (fs::stat(GSP_BOOT_DESC_PATH, &dattr) != fs::OK) {
        log::error("nvidia: bootimg: desc stat failed - is it staged in the initrd?");
        return ERR_BOOTIMG_IO;
    }
    if (dattr.size != sizeof(out.desc)) {
        log::error("nvidia: bootimg: desc size=%lu expected %lu",
                   static_cast<unsigned long>(dattr.size),
                   static_cast<unsigned long>(sizeof(out.desc)));
        return ERR_BOOTIMG_SIZE;
    }
    if (read_whole(GSP_BOOT_DESC_PATH, &out.desc, sizeof(out.desc)) != BOOTIMG_OK) {
        return ERR_BOOTIMG_IO;
    }

    // 2) SK+BL boot image into a CACHED contiguous DMA buffer (matches the open
    //    driver's NV_MEMORY_CACHED gspRmBootUcode memdesc).
    fs::vattr iattr;
    if (fs::stat(GSP_BOOT_IMAGE_PATH, &iattr) != fs::OK) {
        log::error("nvidia: bootimg: image stat failed - is it staged in the initrd?");
        return ERR_BOOTIMG_IO;
    }
    out.image_size = static_cast<uint32_t>(iattr.size);
    if (dma_alloc(out.image_size, /*uncached=*/false, out.image) != MEM_OK) {
        log::error("nvidia: bootimg: image DMA alloc (%u B) failed", out.image_size);
        out.image_size = 0;
        return ERR_BOOTIMG_IO;
    }
    if (read_whole(GSP_BOOT_IMAGE_PATH, reinterpret_cast<void*>(out.image.cpu_va),
                   out.image_size) != BOOTIMG_OK) {
        dma_free(out.image);
        out.image = dma_buffer{};
        out.image_size = 0;
        return ERR_BOOTIMG_IO;
    }

    const rm_riscv_ucode_desc& d = out.desc;
    log::info("nvidia: bootimg: SK+BL image=%u B @ phys=0x%lx; desc ver=%u appVer=%u monEn=%u signedAsCode=%u",
              out.image_size, static_cast<unsigned long>(out.image.phys),
              d.version, d.app_version, d.b_is_monitor_enabled, d.b_signed_as_code);
    log::info("nvidia: bootimg: manifest @0x%x sz=0x%x | monData @0x%x sz=0x%x | monCode @0x%x sz=0x%x",
              d.manifest_offset, d.manifest_size, d.monitor_data_offset, d.monitor_data_size,
              d.monitor_code_offset, d.monitor_code_size);
    log::info("nvidia: bootimg: bootloader @0x%x sz=0x%x | blParam @0x%x sz=0x%x | fbReserved=0x%x",
              d.bootloader_offset, d.bootloader_size, d.bootloader_param_offset,
              d.bootloader_param_size, d.fb_reserved_size);
    return BOOTIMG_OK;
}

void gsp_free_boot_ucode(gsp_boot_ucode& out) {
    dma_free(out.image);
    out.image = dma_buffer{};
    out.image_size = 0;
}

} // namespace nvidia
