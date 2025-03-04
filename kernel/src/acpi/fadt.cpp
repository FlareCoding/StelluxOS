#include <acpi/fadt.h>
#include <memory/memory.h>
#include <core/klog.h>

namespace acpi {
fadt g_fadt;

fadt& fadt::get() {
    return g_fadt;
}

__PRIVILEGED_CODE
void fadt::init(acpi_sdt_header* acpi_fadt_table) {
    memcpy(&m_fadt_data, acpi_fadt_table, sizeof(fadt_table));
}

__PRIVILEGED_CODE
void fadt::reboot() {
    kprint("[REBOOT] Initiating reboot through a FADT acpi table\n");

    uint16_t reset_port = static_cast<uint16_t>(m_fadt_data.reset_reg.address);
    uint8_t reset_value = m_fadt_data.reset_value;
    
    outb(reset_port, reset_value);
}

__PRIVILEGED_CODE
void fadt::shutdown() {
    kprint("[SHUTDOWN] Initiating shutdown through a FADT acpi table\n");

    uint16_t SLP_EN = (1 << 13);  // Sleep enable bit

    outw(m_fadt_data.pm1a_control_block, SLP_EN);

    if (m_fadt_data.pm1b_control_block) {
        outw(m_fadt_data.pm1b_control_block, SLP_EN);
    }
}
} // namespace acpi

