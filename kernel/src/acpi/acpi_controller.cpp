#include "acpi_controller.h"
#include <paging/phys_addr_translation.h>
#include <kstring.h>
#include <memory/kmemory.h>
#include <kprint.h>

__PRIVILEGED_DATA
AcpiController g_acpiController;

__PRIVILEGED_CODE
AcpiController& AcpiController::get() {
    return g_acpiController;
}

#define AML_OPCODE_DEVICE 0x82  // Device opcode
#define AML_OPCODE_NAME 0x08    // Name opcode

__PRIVILEGED_CODE
uint32_t decodePkgLength(uint8_t** pointer) {
    uint8_t byte0 = *(*pointer)++;
    uint32_t pkgLength = byte0 & 0x3F; // Lower 6 bits
    uint32_t bytesAdditional = (byte0 >> 6) & 0x03; // Upper 2 bits

    for (uint32_t i = 0; i < bytesAdditional; ++i) {
        pkgLength |= ((uint32_t)(*(*pointer)++) << (6 + i * 8));
    }

    return pkgLength;
}

__PRIVILEGED_CODE
void parseNameObject(uint8_t** amlPointer) {
    // Skip the NameString (4 bytes)
    *amlPointer += 4;
    
    uint8_t dataType = *(*amlPointer)++;
    switch (dataType) {
        case 0x0A: // ByteConst
        case 0x0B: // WordConst
        case 0x0C: // DWordConst
        case 0x0E: // QWordConst
            // Handle integer (you'll need to read the appropriate number of bytes for each type)
            break;
        case 0x0D: // StringPrefix
        {
            char buffer[256] = {0};
            uint64_t i = 0;
            while ((buffer[i] = (char)*(*amlPointer)) != '\0') {
                // Bounds checking to prevent buffer overflow
                if (i < sizeof(buffer) - 2) {
                    i++;
                }
                (*amlPointer)++;
            }
            buffer[i] = '\0'; // Correctly null-terminate the string
            kprint("             _HID String Found: %s\n", buffer);
            
            // Check for common XHCI _HID strings
            if (memcmp(buffer, (char*)"PNP0D10", 7) == 0 ||
                memcmp(buffer, (char*)"ACPI\\80860F35", 13) == 0) {
                kprint("              XHCI Controller Found: %s\n", buffer);
            }
            break;
        }
        // Add cases for other data types as necessary
        default:
            break;
    }
}

__PRIVILEGED_CODE
void parseDsdt(AcpiTableHeader* dsdt) {
    uint8_t* amlStart = (uint8_t*)(dsdt + 1);
    uint8_t* amlEnd = (uint8_t*)dsdt + dsdt->length;
    uint8_t* amlPointer = amlStart;

    while (amlPointer < amlEnd) {
        uint8_t opcode = *amlPointer++;

        switch (opcode) {
            case AML_OPCODE_DEVICE: {
                uint32_t pkgLength = decodePkgLength(&amlPointer);
                uint8_t* deviceEnd = amlPointer + pkgLength;

                // Now process the contents of the device package
                while (amlPointer < deviceEnd) {
                    if (*amlPointer == AML_OPCODE_NAME) {
                        amlPointer++; // Move past the Name opcode
                        if (memcmp((char*)amlPointer, (char*)"_HID", 4) == 0) {
                            parseNameObject(&amlPointer);
                        } else {
                            // Skip the device name
                            amlPointer += 4;
                        }
                    } else {
                        // Skip other opcodes or parse them as needed
                        amlPointer++; // This is a simplification
                    }
                }

                amlPointer = deviceEnd; // Skip to the end of the Device package
                break;
            }
            // ... handle other opcodes ...
            default:
                // Handle unknown opcode or skip
                break;
        }
    }
}

__PRIVILEGED_CODE
void AcpiController::init(void* rsdp) {
    uint64_t xsdtAddr = static_cast<AcpiRsdp*>(rsdp)->xsdtAddress;
    m_xsdt = reinterpret_cast<AcpiXsdt*>(__va((void*)xsdtAddr));

    m_acpiTableEntries = (m_xsdt->header.length - sizeof(AcpiTableHeader)) / sizeof(uint64_t);

    kprint("Xsdt Addr: 0x%llx\n", m_xsdt);
    kprint("ACPI Entries: %lli\n", m_acpiTableEntries);

    for (size_t i = 0; i < m_acpiTableEntries; ++i) {
        AcpiTableHeader* table = (AcpiTableHeader *)m_xsdt->tablePointers[i];
        char tableName[5];
        tableName[4] = '\0';
        memcpy(tableName, table->signature, 4);

        kprint("   ACPI Table Entry Found: %s\n", tableName);

        if (memcmp(table->signature, (char*)"FACP", 4) == 0) {
            // We've found the FACP, now let's get the DSDT
            AcpiFacp* facp = (AcpiFacp*)table;
            uint64_t dsdtAddress = facp->x_dsdt;  // Use the 64-bit address for ACPI 2.0+
            AcpiTableHeader* dsdt = reinterpret_cast<AcpiTableHeader*>(__va((void*)dsdtAddress));
            
            kprint("       DSDT Address: 0x%llx\n", (unsigned long long)dsdt);
            //parseDsdt(dsdt);
        } else if (memcmp(table->signature, (char*)"SSDT", 4) == 0) {
            //parseDsdt(table);
        }
    }
    kprint("\n");
}
