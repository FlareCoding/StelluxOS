#include "hw/rtc.h"
#include "common/logging.h"

#if !defined(STLX_PLATFORM_RPI4)
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "hw/mmio.h"
#endif

namespace rtc {

static uint64_t g_boot_unix_ns;

constexpr uint64_t NS_PER_SEC = 1000000000ULL;

#if defined(STLX_PLATFORM_RPI4)

__PRIVILEGED_CODE int32_t init() {
#ifdef STLX_BUILD_EPOCH
    g_boot_unix_ns = static_cast<uint64_t>(STLX_BUILD_EPOCH) * NS_PER_SEC;
    log::warn("rtc: no hardware RTC, using build-time epoch (%lu)",
              static_cast<uint64_t>(STLX_BUILD_EPOCH));
#else
    g_boot_unix_ns = 0;
    log::warn("rtc: no hardware RTC and no build epoch");
#endif
    return OK;
}

#else // QEMU virt — PL031 RTC

constexpr uintptr_t PL031_PHYS = 0x09010000;
constexpr size_t    PL031_SIZE = 0x1000;
constexpr uintptr_t PL031_RTCDR_OFFSET = 0x00;

__PRIVILEGED_CODE int32_t init() {
    uintptr_t kva_base = 0;
    uintptr_t rtc_va = 0;

    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(PL031_PHYS),
        PL031_SIZE,
        paging::PAGE_KERNEL_RW,
        kva_base,
        rtc_va);
    if (rc != vmm::OK) {
        log::error("rtc: failed to map PL031 at 0x%lx", PL031_PHYS);
        return ERR;
    }

    uint32_t unix_sec = mmio::read32(rtc_va + PL031_RTCDR_OFFSET);
    g_boot_unix_ns = static_cast<uint64_t>(unix_sec) * NS_PER_SEC;

    log::info("rtc: PL031 epoch=%u sec", unix_sec);
    return OK;
}

#endif // STLX_PLATFORM_RPI4

uint64_t boot_unix_ns() {
    return g_boot_unix_ns;
}

} // namespace rtc
