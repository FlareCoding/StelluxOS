#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "pci/pci.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(pci_test);

// pci::init() is called from boot.cpp before tests run, so the subsystem
// is already initialized. Tests verify the discovered state.

TEST(pci_test, init_succeeds) {
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = pci::init();
    });
    EXPECT_EQ(rc, pci::OK);
}

TEST(pci_test, device_count_nonzero) {
    EXPECT_GT(pci::device_count(), static_cast<uint32_t>(0));
}

TEST(pci_test, find_host_bridge) {
    pci::device* dev = pci::find_by_class(0x06, 0x00);
    EXPECT_NOT_NULL(dev);
}

TEST(pci_test, find_xhci) {
    pci::device* dev = pci::find_by_progif(0x0C, 0x03, 0x30);
    EXPECT_NOT_NULL(dev);
}

TEST(pci_test, xhci_bar0_valid) {
    pci::device* dev = pci::find_by_progif(0x0C, 0x03, 0x30);
    if (!dev) return;

    const pci::bar& b = dev->get_bar(0);
    EXPECT_TRUE(b.type == pci::BAR_MMIO32 || b.type == pci::BAR_MMIO64);
    EXPECT_GT(b.size, static_cast<uint64_t>(0));
    EXPECT_NE(b.phys, static_cast<uint64_t>(0));
}

TEST(pci_test, xhci_has_msix) {
    pci::device* dev = pci::find_by_progif(0x0C, 0x03, 0x30);
    if (!dev) return;

    EXPECT_TRUE(dev->has_capability(pci::CAP_MSIX));
}

TEST(pci_test, config_read_vendor) {
    pci::device* dev = pci::get_device(0);
    if (!dev) return;

    uint16_t live_vendor = 0;
    RUN_ELEVATED({
        live_vendor = dev->config_read16(pci::CFG_VENDOR_ID);
    });
    EXPECT_EQ(live_vendor, dev->vendor_id());
}

TEST(pci_test, device_index_bounds) {
    pci::device* dev = pci::get_device(pci::device_count());
    EXPECT_NULL(dev);
}

TEST(pci_test, bar_alignment) {
    for (uint32_t i = 0; i < pci::device_count(); i++) {
        pci::device* dev = pci::get_device(i);
        if (!dev) continue;

        for (uint8_t b = 0; b < pci::MAX_BARS; b++) {
            const pci::bar& bar = dev->get_bar(b);
            if (bar.type == pci::BAR_NONE || bar.size == 0) continue;
            EXPECT_EQ(bar.phys % bar.size, static_cast<uint64_t>(0));
        }
    }
}
