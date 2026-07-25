#include "drivers/gpu/nvidia/nv_radix3.h"
#include "fs/fs.h"
#include "hw/barrier.h"
#include "common/logging.h"

namespace nvidia {

constexpr uint32_t RADIX_PAGE      = 4096;
constexpr uint32_t RADIX_LOG2      = 12;
constexpr uint32_t RADIX_ENTRIES_LOG2 = 9; // 4096 / 8 bytes = 512 entries/page

// Read [off, off+size) of the firmware file into dst (chunked).
static int32_t load_fwimage(const char* path, uint64_t off, uint64_t size, uint8_t* dst) {
    int32_t oerr = 0;
    fs::file* f = fs::open(path, fs::O_RDONLY, &oerr);
    if (!f) {
        log::error("nvidia: radix3: open(%s) failed err=%d", path, oerr);
        return ERR_RADIX3_IO;
    }
    int32_t rc = ERR_RADIX3_IO;
    if (fs::seek(f, static_cast<int64_t>(off), fs::SEEK_SET) >= 0) {
        uint64_t done = 0;
        rc = RADIX3_OK;
        while (done < size) {
            const size_t chunk = (size - done > (1u << 20)) ? (1u << 20)
                                                            : static_cast<size_t>(size - done);
            const ssize_t got = fs::read(f, dst + done, chunk);
            if (got <= 0) { rc = ERR_RADIX3_IO; break; }
            done += static_cast<uint64_t>(got);
        }
    }
    fs::close(f);
    return rc;
}

int32_t gsp_build_radix3(const char* fw_path, const gsp_fw_image& fi, gsp_radix3& out) {
    out.image = dma_buffer{};
    out.table = dma_buffer{};
    out.root_phys = 0;
    out.data_pages = 0;

    const uint64_t size = fi.fwimage_size;
    if (size == 0) {
        return ERR_RADIX3_SHAPE;
    }

    // Working array (radix3[0]=root .. radix3[3]=data), high->low for nPages.
    uint64_t nPages[4] = {0, 0, 0, 0};
    uint64_t offset[4] = {0, 0, 0, 0};
    nPages[3] = (size + RADIX_PAGE - 1) >> RADIX_LOG2;
    for (int i = 3; i > 0; i--) {
        nPages[i - 1] = ((nPages[i] - 1) >> RADIX_ENTRIES_LOG2) + 1;
    }
    if (nPages[0] != 1) {
        log::error("nvidia: radix3: root pages != 1 (%lu)", static_cast<unsigned long>(nPages[0]));
        return ERR_RADIX3_SHAPE;
    }
    uint64_t acc = 0;
    for (int i = 1; i < 4; i++) {
        acc += nPages[i - 1];
        offset[i] = acc << RADIX_LOG2;
    }
    const uint64_t ptPages = acc;                 // page-table pages (levels 0..2)
    const uint64_t ptSize  = ptPages << RADIX_LOG2;

    // Allocate the GSP-RM image (contiguous, CACHED) and load .fwimage into it.
    if (dma_alloc(size, /*uncached=*/false, out.image) != MEM_OK) {
        log::error("nvidia: radix3: image alloc (%lu B) failed", static_cast<unsigned long>(size));
        return ERR_RADIX3_MEM;
    }
    if (load_fwimage(fw_path, fi.fwimage_off, size,
                     reinterpret_cast<uint8_t*>(out.image.cpu_va)) != RADIX3_OK) {
        dma_free(out.image);
        return ERR_RADIX3_IO;
    }

    // Allocate the page table (contiguous, CACHED, zeroed).
    if (dma_alloc(ptSize, /*uncached=*/false, out.table) != MEM_OK) {
        log::error("nvidia: radix3: table alloc (%lu B) failed", static_cast<unsigned long>(ptSize));
        dma_free(out.image);
        return ERR_RADIX3_MEM;
    }

    uint64_t* pt = reinterpret_cast<uint64_t*>(out.table.cpu_va);
    const uint64_t tphys = out.table.phys;
    const uint64_t dphys = out.image.phys;

    // Root PDE (1 entry) -> level-1 page.
    pt[(offset[0] >> 3)] = tphys + offset[1];
    // Level-1 PDEs -> level-2 (leaf) pages.
    for (uint64_t j = 0; j < nPages[2]; j++) {
        pt[(offset[1] >> 3) + j] = tphys + offset[2] + j * RADIX_PAGE;
    }
    // Leaf PTEs -> the data (.fwimage) pages.
    for (uint64_t k = 0; k < nPages[3]; k++) {
        pt[(offset[2] >> 3) + k] = dphys + k * RADIX_PAGE;
    }

    barrier::dma_full(); // ensure the table is in memory before the GPU reads it

    out.root_phys = tphys;
    out.data_pages = static_cast<uint32_t>(nPages[3]);

    log::info("nvidia: radix3: .fwimage=%lu B (%lu pages), table=%lu pages, root=0x%lx imagePhys=0x%lx",
              static_cast<unsigned long>(size), static_cast<unsigned long>(nPages[3]),
              static_cast<unsigned long>(ptPages), static_cast<unsigned long>(tphys),
              static_cast<unsigned long>(dphys));
    log::info("nvidia: radix3: levels nPages[0..3]=%lu/%lu/%lu/%lu offsets=0x%lx/0x%lx/0x%lx/0x%lx",
              static_cast<unsigned long>(nPages[0]), static_cast<unsigned long>(nPages[1]),
              static_cast<unsigned long>(nPages[2]), static_cast<unsigned long>(nPages[3]),
              static_cast<unsigned long>(offset[0]), static_cast<unsigned long>(offset[1]),
              static_cast<unsigned long>(offset[2]), static_cast<unsigned long>(offset[3]));
    log::info("nvidia: radix3: root[0]=0x%lx l1[0]=0x%lx leaf[0]=0x%lx leaf[%lu]=0x%lx",
              static_cast<unsigned long>(pt[offset[0] >> 3]),
              static_cast<unsigned long>(pt[offset[1] >> 3]),
              static_cast<unsigned long>(pt[offset[2] >> 3]),
              static_cast<unsigned long>(nPages[3] - 1),
              static_cast<unsigned long>(pt[(offset[2] >> 3) + (nPages[3] - 1)]));
    return RADIX3_OK;
}

void gsp_free_radix3(gsp_radix3& out) {
    dma_free(out.table);
    dma_free(out.image);
    out.root_phys = 0;
    out.data_pages = 0;
}

} // namespace nvidia
