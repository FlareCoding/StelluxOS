#include "xhci_device_ctx.h"
#include <kprint.h>

void XhciDeviceContextManager::allocateDcbaa(XhciHcContext* xhc) {
    size_t contextEntrySize = xhc->has64ByteContextSize() ? 64 : 32;
    size_t dcbaaSize = contextEntrySize * (xhc->getMaxDeviceSlots() + 1);

    m_dcbaa = xhciAllocDma<uint64_t>(dcbaaSize, XHCI_DEVICE_CONTEXT_ALIGNMENT, XHCI_DEVICE_CONTEXT_BOUNDARY);

    /*
    // xHci Spec Section 6.1 (page 404)

    If the Max Scratchpad Buffers field of the HCSPARAMS2 register is > ‘0’, then
    the first entry (entry_0) in the DCBAA shall contain a pointer to the Scratchpad
    Buffer Array. If the Max Scratchpad Buffers field of the HCSPARAMS2 register is
    = ‘0’, then the first entry (entry_0) in the DCBAA is reserved and shall be
    cleared to ‘0’ by software.
    */

    // Initialize scratchpad buffer array if needed
    uint8_t scratchpadBuffers = xhc->getMaxScratchpadBuffers();
    if (scratchpadBuffers > 0) {
        // Array of uint64_t pointers
        XhciDma<uint64_t> scratchpadArray = xhciAllocDma<uint64_t>(scratchpadBuffers * sizeof(uint64_t));
        
        // Create scratchpad pages
        for (uint8_t i = 0; i < scratchpadBuffers; i++) {
            XhciDma<> scratchpad = xhciAllocDma(PAGE_SIZE, XHCI_SCRATCHPAD_BUFFERS_ALIGNMENT, XHCI_SCRATCHPAD_BUFFERS_BOUNDARY);
            scratchpadArray.virtualBase[i] = scratchpad.physicalBase;
        }

        // Set the first slot in the DCBAA to point to the scratchpad array
        m_dcbaa.virtualBase[0] = scratchpadArray.physicalBase;
    }

    // Update the DCBAAP entry in operational registers
    xhc->opRegs->dcbaap = m_dcbaa.physicalBase;
}

void XhciDeviceContextManager::allocateDeviceContext(XhciHcContext* xhc, uint8_t slot) const {
    // Allocate a memory block for the device context
    if (xhc->has64ByteContextSize()) {
        auto ctx = xhciAllocDma<XhciDeviceContext64>(
            sizeof(XhciDeviceContext64),
            XHCI_DEVICE_CONTEXT_ALIGNMENT,
            XHCI_DEVICE_CONTEXT_BOUNDARY
        );

        m_dcbaa.virtualBase[slot] = ctx.physicalBase;
    } else {
        auto ctx = xhciAllocDma<XhciDeviceContext32>(
            sizeof(XhciDeviceContext32),
            XHCI_DEVICE_CONTEXT_ALIGNMENT,
            XHCI_DEVICE_CONTEXT_BOUNDARY
        );

        m_dcbaa.virtualBase[slot] = ctx.physicalBase;
    }
}
