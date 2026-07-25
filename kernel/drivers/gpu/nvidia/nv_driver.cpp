#include "drivers/pci_driver.h"
#include "drivers/gpu/nvidia/nv_regs.h"
#include "drivers/gpu/nvidia/nv_gpu.h"
#include "pci/pci.h"
#include "common/logging.h"
#include "mm/heap.h"

// === Step 4.1 -- PCI-driver SCAFFOLD (no behavior change) ===
//
// Stand up the NVIDIA GA102 as a first-class PCI driver (a peer of `xhci_hcd`), auto-discovered by
// the driver framework via REGISTER_PCI_DRIVER. For now attach()/run()/on_interrupt() are
// LOGGING-ONLY STUBS: the real cold-boot bring-up still runs through nvidia::init() called inline
// from boot/boot.cpp. Because this driver never touches the device, the two coexist with zero
// conflict -- nvidia::init() still owns BAR0 + the GSP/display bring-up.
//
// Purpose: prove the framework discovers + binds the GPU (the "attach" log) while the working
// baseline (cold boot -> 2560x1440@144 + HW cursor) is completely untouched. Subsequent steps
// migrate the bring-up + interrupts into this driver:
//   4.2 run the bring-up in this driver's task (off the idle thread -> interrupts go live)
//   4.3 framework device + map_bar   4.4 framework setup_msi + on_interrupt   4.5 retire nvidia::init()
// See IMPLEMENTATION_NOTES.md "STEP 4 -- PCI DRIVER MIGRATION".
namespace nvidia {

class nvidia_gpu : public drivers::pci_driver {
public:
    nvidia_gpu(pci::device* dev) : drivers::pci_driver("nvidia_gpu", dev) {}

    int32_t attach() override {
        log::info("nvidia: [pci_driver] attach: bound %02x:%02x.%x (vendor=0x%04x device=0x%04x class=%02x:%02x) "
                  "-- Step 4.1 scaffold; real bring-up still via nvidia::init()",
                  dev().bus(), dev().slot(), dev().func(),
                  dev().vendor_id(), dev().device_id(), dev().class_code(), dev().subclass());
        return drivers::OK;
    }

    void run() override {
        // Run the GSP/display bring-up on THIS framework-spawned driver task. The bring-up is a
        // synchronous boot that busy-polls the GSP status ring (delay::us) -- like the xhci HCD polls
        // during reset/port init -- because the first CPU-sequencer opcode is a time-critical MAILBOX0
        // handshake the receive path must not yield. nvidia::init() drives the whole bring-up off the
        // framework-provided pci::device (maps BAR0 PAGE_USER, etc.).
        log::info("nvidia: [pci_driver] run: starting GSP/display bring-up on driver task");
        const int32_t rc = nvidia::init(&dev()); // framework-bound device (no privileged find_gpu scan)
        log::info("nvidia: [pci_driver] run: bring-up returned %d -- parking task", rc);
        while (true) {
            wait_for_event();
        }
    }

    __PRIVILEGED_CODE void on_interrupt(uint32_t /*vector*/) override {
        // This driver owns no MSI yet (added in Step 4.4) -- nothing to service.
    }
};

REGISTER_PCI_DRIVER(nvidia_gpu,
    PCI_MATCH(PCI_VENDOR_NVIDIA, PCI_DEVICE_GA102_RTX3080,
              ::drivers::PCI_MATCH_ANY_8, ::drivers::PCI_MATCH_ANY_8, ::drivers::PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(nvidia_gpu));

} // namespace nvidia
