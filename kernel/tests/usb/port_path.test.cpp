#include "stlx_unit_test.h"
#include "drivers/usb/xhci/xhci_device.h"
#include "mm/heap.h"

TEST_SUITE(usb_port_path);

using drivers::xhci::xhci_device;

static constexpr uint8_t ROOT_PORT = 5;

// The largest port a route string's four bits hold, and the last port of the largest hub
static constexpr uint8_t LAST_ROUTE_STRING_PORT = 15;
static constexpr uint8_t LAST_HUB_PORT = xhci_device::MAX_HUB_PORTS;

// The most hubs USB allows between a root port and a device
static constexpr size_t MAX_HUBS = usb::MAX_PORT_PATH_LENGTH - 1;

// The five hubs of a chain, the device on the last hub and a device below that one
static constexpr size_t DEVICE_COUNT = MAX_HUBS + 2;

static xhci_device* g_devices[DEVICE_COUNT];

static int32_t allocate_devices() {
    for (size_t i = 0; i < DEVICE_COUNT; i++) {
        g_devices[i] = heap::ualloc_new<xhci_device>();
        if (!g_devices[i]) {
            return -1;
        }
    }

    return 0;
}

static int32_t free_devices() {
    for (size_t i = 0; i < DEVICE_COUNT; i++) {
        heap::ufree_delete(g_devices[i]);
    }

    return 0;
}

BEFORE_ALL(usb_port_path, allocate_devices);
AFTER_ALL(usb_port_path, free_devices);

TEST(usb_port_path, ports_15_and_16_of_a_hub_get_paths_of_their_own) {
    xhci_device* hub = g_devices[0];
    ASSERT_EQ(hub->set_port_path(nullptr, ROOT_PORT), 0);
    ASSERT_EQ(g_devices[1]->set_port_path(hub, LAST_ROUTE_STRING_PORT), 0);
    ASSERT_EQ(g_devices[2]->set_port_path(hub, LAST_HUB_PORT), 0);

    ASSERT_EQ(hub->port_path_length(), 1u);
    EXPECT_EQ(hub->port_path()[0], ROOT_PORT);

    ASSERT_EQ(g_devices[1]->port_path_length(), 2u);
    EXPECT_EQ(g_devices[1]->port_path()[0], ROOT_PORT);
    EXPECT_EQ(g_devices[1]->port_path()[1], LAST_ROUTE_STRING_PORT);

    ASSERT_EQ(g_devices[2]->port_path_length(), 2u);
    EXPECT_EQ(g_devices[2]->port_path()[0], ROOT_PORT);
    EXPECT_EQ(g_devices[2]->port_path()[1], LAST_HUB_PORT);
}

// The port on each hub of the chain, distinct so a path copied out of order shows
static constexpr uint8_t CHAIN_HUB_PORTS[MAX_HUBS] = {12, 13, 14, LAST_ROUTE_STRING_PORT, LAST_HUB_PORT};

TEST(usb_port_path, a_path_holds_five_hubs_and_refuses_a_sixth) {
    ASSERT_EQ(g_devices[0]->set_port_path(nullptr, ROOT_PORT), 0);
    for (size_t i = 0; i < MAX_HUBS; i++) {
        ASSERT_EQ(g_devices[i + 1]->set_port_path(g_devices[i], CHAIN_HUB_PORTS[i]), 0);
    }

    const xhci_device* deepest = g_devices[MAX_HUBS];
    ASSERT_EQ(deepest->port_path_length(), usb::MAX_PORT_PATH_LENGTH);
    EXPECT_EQ(deepest->port_path()[0], ROOT_PORT);
    for (size_t i = 0; i < MAX_HUBS; i++) {
        EXPECT_EQ(deepest->port_path()[i + 1], CHAIN_HUB_PORTS[i]);
    }

    xhci_device* refused = g_devices[MAX_HUBS + 1];
    EXPECT_EQ(refused->set_port_path(deepest, LAST_HUB_PORT), -1);
    EXPECT_EQ(refused->port_path_length(), 0u);
}
