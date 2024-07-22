#include "xhci.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <paging/tlb.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <arch/x86/ioapic.h>
#include <interrupts/interrupts.h>
#include <kprint.h>

namespace drivers {
    XhciDriver g_globalXhciInstance;

    XhciDriver& XhciDriver::get() {
        return g_globalXhciInstance;
    }

    bool XhciDriver::init(PciDeviceInfo& deviceInfo) {
        _mapDeviceMmio(deviceInfo.barAddress);

        kuPrint("\n");
        return true;
    }

    void XhciDriver::_mapDeviceMmio(uint64_t pciBarAddress) {
        // Map a conservatively large space for xHCI registers
        for (size_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
            void* mmioPage = (void*)(pciBarAddress + offset);
            paging::mapPage(mmioPage, mmioPage, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::g_kernelRootPageTable);
        }

        paging::flushTlbAll();

        m_xhcBase = pciBarAddress;
    }

    void* XhciDriver::_allocXhciMemory(size_t size) {
        void* ptr = kmallocAligned(size, 64);
        if (!ptr) {
            kuPrint("[XHCI] ======= MEMORY ALLOCATION PROBLEM =======\n");
            
            // Ideally panic, but spin for now
            while (true);
        }

        // Make sure the memory is uncacheable
        paging::markPageUncacheable(ptr);

        return ptr;
    }
} // namespace drivers
