#ifndef FADT_H
#define FADT_H
#include "acpi.h"

namespace acpi {
struct generic_address_structure {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

/**
 * @struct fadt_table
 * @brief Represents the Fixed ACPI Description Table (FADT).
 *
 * The FADT contains information about power management hardware and the system's sleep states.
 * It is a key component in ACPI-compliant systems.
 */
struct fadt_table {
    acpi_sdt_header header;

    uint32_t firmware_ctrl;
    uint32_t dsdt;

    // Field used in ACPI 1.0, but reserved in later versions
    uint8_t reserved;

    uint8_t preferred_power_management_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_ctrl_len;
    uint8_t  pm2_ctrl_len;
    uint8_t  pm_timer_len;
    uint8_t  gpe0_block_len;
    uint8_t  gpe1_block_len;
    uint8_t gpe1_base;
    uint8_t cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;

    // Reserved in ACPI 1.0, used since ACPI 2.0+
    uint16_t iapc_boot_arch_flags;

    uint8_t reserved2;
    uint32_t flags;

    generic_address_structure reset_reg;

    uint8_t reset_value;
    uint8_t reserved3[3];

    // 64-bit pointers available on ACPI 2.0+
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;

    generic_address_structure x_pm1a_event_block;
    generic_address_structure x_pm1b_event_block;
    generic_address_structure x_pm1a_ctrl_block;
    generic_address_structure x_pm1b_ctrl_block;
    generic_address_structure x_pm_timer_block;
    generic_address_structure x_gpe0_block;
    generic_address_structure x_gpe1_block;
} __attribute__((packed));

/**
 * @class fadt
 * @brief Manages the Fixed ACPI Description Table (FADT) and provides access to power management features.
 *
 * This class is responsible for parsing and storing the FADT information.
 * It provides methods to retrieve power management-related settings and system capabilities.
 */
class fadt {
public:
    /**
     * @brief Gets the singleton instance of the fadt class.
     * @return The singleton fadt instance.
     *
     * Ensures only one instance of the fadt class is used, providing a global point of access.
     */
    static fadt& get();

    /**
     * @brief Default constructor for the fadt class.
     */
    fadt() = default;

    /**
     * @brief Default destructor for the fadt class.
     */
    ~fadt() = default;

    /**
     * @brief Initializes the fadt object using the provided ACPI FADT table.
     * @param acpi_fadt_table Pointer to the ACPI FADT table header.
     *
     * Processes the provided FADT table and extracts relevant power management settings.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void init(acpi_sdt_header* acpi_fadt_table);

    /**
     * @brief Returns the SCI interrupt number.
     * @return The System Control Interrupt (SCI) number.
     *
     * The SCI is used for ACPI power management events.
     */
    __force_inline__ uint16_t get_sci_interrupt() const { return m_fadt_data.sci_interrupt; }

    /**
     * @brief Retrieves the iAPC boot architecture flags.
     * @return The boot architecture flags.
     */
    __force_inline__ uint16_t get_iapc_boot_arch_flags() const { return m_fadt_data.iapc_boot_arch_flags; }

    /**
     * @brief Checks if a PS/2 controller is present.
     * @return True if a PS/2 controller is present, false otherwise.
     */
    __force_inline__ bool is_ps2_controller_present() const { return !(m_fadt_data.iapc_boot_arch_flags & (1 << 1)); }
    
    /**
     * @brief Reboots the system using the ACPI reset register.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void reboot();
    
    /**
     * @brief Shuts down the system using ACPI S5 sleep state.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void shutdown();

private:
    fadt_table m_fadt_data; // Stores the parsed FADT table data.
};
} // namespace acpi
#endif // FADT_H
