#ifndef STELLUX_DRIVERS_NET_PHY_REGS_H
#define STELLUX_DRIVERS_NET_PHY_REGS_H

/**
 * Standard IEEE 802.3 MII PHY registers and Broadcom BCM54213PE
 * shadow registers for RGMII clock delay configuration.
 */

#include "common/types.h"

namespace drivers::phy {

// ============================================================================
// Standard MII registers (IEEE 802.3)
// ============================================================================

// Basic Control Register
constexpr uint8_t  BMCR                      = 0x00;
constexpr uint16_t BMCR_RESET                = (1u << 15);
constexpr uint16_t BMCR_LOOPBACK             = (1u << 14);
constexpr uint16_t BMCR_SPEED_100            = (1u << 13);
constexpr uint16_t BMCR_ANE                  = (1u << 12);  // Auto-Negotiation Enable
constexpr uint16_t BMCR_POWER_DOWN           = (1u << 11);
constexpr uint16_t BMCR_ISOLATE              = (1u << 10);
constexpr uint16_t BMCR_RESTART_AN           = (1u << 9);   // Restart Auto-Negotiation
constexpr uint16_t BMCR_FULL_DUPLEX          = (1u << 8);
constexpr uint16_t BMCR_SPEED_1000           = (1u << 6);

// Basic Status Register
constexpr uint8_t  BMSR                      = 0x01;
constexpr uint16_t BMSR_100BASET4            = (1u << 15);
constexpr uint16_t BMSR_100BASETX_FDX        = (1u << 14);
constexpr uint16_t BMSR_100BASETX            = (1u << 13);
constexpr uint16_t BMSR_10BASET_FDX          = (1u << 12);
constexpr uint16_t BMSR_10BASET              = (1u << 11);
constexpr uint16_t BMSR_ANEG_COMPLETE        = (1u << 5);
constexpr uint16_t BMSR_LINK_STATUS          = (1u << 2);

// PHY Identifier registers
constexpr uint8_t  PHYIDR1                   = 0x02;
constexpr uint8_t  PHYIDR2                   = 0x03;

// Auto-Negotiation Advertisement Register
constexpr uint8_t  ANAR                      = 0x04;
constexpr uint16_t ANAR_100BASETX_FDX        = (1u << 8);
constexpr uint16_t ANAR_100BASETX            = (1u << 7);
constexpr uint16_t ANAR_10BASET_FDX          = (1u << 6);
constexpr uint16_t ANAR_10BASET              = (1u << 5);

// Auto-Negotiation Link Partner Ability Register
constexpr uint8_t  ANLPAR                    = 0x05;
// Same bit layout as ANAR

// 1000Base-T Control Register
constexpr uint8_t  GBCR                      = 0x09;
constexpr uint16_t GBCR_1000BASET_FDX        = (1u << 9);
constexpr uint16_t GBCR_1000BASET            = (1u << 8);

// 1000Base-T Status Register
constexpr uint8_t  GBSR                      = 0x0A;
// Partner bits are shifted right by 2 from GBCR bits
constexpr uint16_t GBSR_1000BASET_FDX        = (1u << 11);
constexpr uint16_t GBSR_1000BASET            = (1u << 10);

// ============================================================================
// Broadcom BCM54213PE specific shadow registers
// Used for RGMII clock delay configuration.
// ============================================================================

// Auxiliary Control shadow register (register 0x18)
// Bits [2:0] select the shadow function (0x00-0x07).
constexpr uint8_t  BRGPHY_AUXCTL             = 0x18;
constexpr uint16_t BRGPHY_AUXCTL_SHADOW_MISC = 0x07;
constexpr uint16_t BRGPHY_AUXCTL_MISC_DATA_MASK = 0x7FF8;
constexpr uint16_t BRGPHY_AUXCTL_MISC_READ_SHIFT = 12;
constexpr uint16_t BRGPHY_AUXCTL_MISC_WRITE_EN = 0x8000;
constexpr uint16_t BRGPHY_AUXCTL_MISC_RGMII_SKEW_EN = 0x0200;

// Shadow register 0x1C
// Bit 15 is write enable, bits [14:10] select function (0x00-0x1F).
constexpr uint8_t  BRGPHY_SHADOW_1C          = 0x1C;
constexpr uint16_t BRGPHY_SHADOW_1C_WRITE_EN = 0x8000;
constexpr uint16_t BRGPHY_SHADOW_1C_SEL_MASK = 0x7C00;
constexpr uint16_t BRGPHY_SHADOW_1C_DATA_MASK = 0x03FF;

// Clock Alignment Control Register (shadow 0x1C, select value 0x03)
constexpr uint16_t BRGPHY_SHADOW_1C_CLK_CTRL = (0x03 << 10);
constexpr uint16_t BRGPHY_SHADOW_1C_GTXCLK_EN = 0x0200;

// ============================================================================
// PHY speed/duplex enumerations
// ============================================================================

enum class phy_speed : uint16_t {
    SPEED_NONE = 0,
    SPEED_10   = 10,
    SPEED_100  = 100,
    SPEED_1000 = 1000,
};

enum class phy_duplex : uint8_t {
    HALF = 0,
    FULL = 1,
};

} // namespace drivers::phy

#endif // STELLUX_DRIVERS_NET_PHY_REGS_H
