#ifndef PCI_MSI_H
#define PCI_MSI_H
#include <types.h>

namespace pci {
/**
 * @brief Base address for local APIC in MSI message address (for xAPIC mode).
 * 
 * According to the Intel specs, MSI messages are typically delivered to 0xFEE0'0000.
 * Bits [19:12] often store the 8-bit destination APIC ID in xAPIC mode.
 * 
 * For x2APIC, you would use a different scheme.
 */
constexpr uint32_t MSI_ADDRESS_BASE = 0xFEE00000;

/**
 * @brief Offsets/bits in the MSI data (message_data).
 * 
 * Bits 7..0:   Interrupt Vector
 * Bits 10..8:  Delivery Mode
 * Bit  14:     Level
 * Bit  15:     Trigger Mode
 * etc.
 */
constexpr uint16_t MSI_DELIVERY_MODE_FIXED  = (0 << 8);
constexpr uint16_t MSI_DELIVERY_MODE_LOWEST = (1 << 8);

/**
 * @brief Builds a 32-bit or 64-bit MSI address for xAPIC mode.
 * 
 * Usually:
 *   - Bits [31..20] are reserved or fixed.
 *   - Bits [19..12] contain the 8-bit APIC ID for the CPU you want the interrupt on.
 */
inline uint64_t build_msi_address(uint8_t cpu_apic_id) {
    // Place APIC ID in bits [19..12].
    uint32_t addr_lo = MSI_ADDRESS_BASE | (static_cast<uint32_t>(cpu_apic_id) << 12);
    // For 32-bit addresses, the high part is 0.
    return static_cast<uint64_t>(addr_lo);
}

/**
 * @brief Builds the MSI data word, which includes the vector, delivery mode, etc.
 * 
 * @param vector The 8-bit interrupt vector you want on that CPU.
 * @param delivery_mode The 3-bit delivery mode (usually 0 = fixed).
 * @return 16-bit message data to write to the MSI capability's message_data.
 */
inline uint16_t build_msi_data(uint8_t vector, uint16_t delivery_mode = MSI_DELIVERY_MODE_FIXED) {
    uint16_t data = 0;
    data |= (vector & 0xFF);      // Bits [7..0] = vector
    data |= delivery_mode;        // Bits [10..8] = delivery mode (fixed/lowest/etc.)
    // For a simple scenario, we won't set level or trigger mode bits here.
    return data;
}

/**
 * @brief Builds the MSI data word, including:
 *   - Vector (bits [7..0])
 *   - Delivery mode (bits [10..8])
 *   - Level (bit 14) and trigger (bit 15)
 * 
 * @param vector       The IDT vector (0..255).
 * @param edgetrigger  1 => edge-triggered (bit 15 = 0), 0 => level-triggered (bit 15 = 1).
 * @param deassert     1 => deassert (bit 14 = 0), 0 => assert (bit 14 = 1).
 * @param delivery_mode Bits [10..8]; typically MSI_DELIVERY_MODE_FIXED or MSI_DELIVERY_MODE_LOWEST.
 * @return 16-bit MSI data register value.
 */
inline uint16_t build_msi_data(uint8_t vector, uint8_t edgetrigger, uint8_t deassert, uint16_t delivery_mode = MSI_DELIVERY_MODE_FIXED) {
    // Bits [7..0] = vector
    uint16_t data = vector & 0xFF;

    // Bits [10..8] = delivery mode
    data |= delivery_mode;

    // Bit 15 => trigger mode: 0 = edge, 1 = level
    // If edgetrigger=1 => we want edge => bit15=0
    // If edgetrigger=0 => we want level => bit15=1
    if (!edgetrigger) {
        data |= (1 << 15);
    }

    // Bit 14 => level: 0 = deassert, 1 = assert
    // If deassert=1 => bit14=0
    // If deassert=0 => bit14=1
    if (!deassert) {
        data |= (1 << 14);
    }

    return data;
}

struct pci_msi_capability {
    union {
        struct {
            uint8_t cap_id;
            uint8_t next_cap_ptr;
            union {
                struct {
                    uint16_t enable_bit               : 1;
                    uint16_t multiple_message_capable : 3;
                    uint16_t multiple_message_enable  : 3;
                    uint16_t is_64bit                 : 1;
                    uint16_t per_vector_masking       : 1;
                    uint16_t rsvd0                    : 7;
                } __attribute__((packed));
                uint16_t message_control;
            };
        } __attribute__((packed));
        uint32_t dword0;
    };
    // Message Address (32 or 64 bits)
    union {
        struct {
            uint32_t message_address_lo;  // Message Address Lower 32 bits
            uint32_t message_address_hi;  // Message Address Upper 32 bits (if 64-bit capable)
        } __attribute__((packed));
        uint64_t message_address;         // Full 64-bit Message Address
    };
    uint16_t message_data;
    uint16_t rsvd1;
    uint32_t mask;
    uint32_t pending;
} __attribute__((packed));
static_assert(sizeof(pci_msi_capability) == 24);
} // namespace pci

#endif // PCI_MSI_H
