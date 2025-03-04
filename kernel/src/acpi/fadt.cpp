#include <acpi/fadt.h>
#include <memory/memory.h>

namespace acpi {
fadt g_fadt;

fadt& fadt::get() {
    return g_fadt;
}

__PRIVILEGED_CODE
void fadt::init(acpi_sdt_header* acpi_fadt_table) {
    memcpy(&m_fadt_data, acpi_fadt_table, sizeof(fadt_table));
}
} // namespace acpi

