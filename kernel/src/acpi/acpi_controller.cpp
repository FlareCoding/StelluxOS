#include "acpi_controller.h"
#include <paging/phys_addr_translation.h>
#include <kstring.h>
#include <memory/kmemory.h>
#include <pci/pci.h>
#include <kprint.h>

__PRIVILEGED_DATA
AcpiController g_acpiController;

__PRIVILEGED_CODE
AcpiController& AcpiController::get() {
    return g_acpiController;
}

#define AML_OPCODE_DEVICE 0x82  // Device opcode
#define AML_OPCODE_NAME 0x08    // Name opcode

#define ACPI_AML_OPCODE_SCOPE 0x10

__PRIVILEGED_CODE
uint32_t parsePkgLength(uint8_t*& amlPointer) {
    uint32_t package_length = 0;
	uint32_t byte_count;
	uint8_t  byte_zero_mask = 0x3F;	/* Default [0:5] */
	/*
	 * Byte 0 bits [6:7] contain the number of additional bytes
	 * used to encode the package length, either 0,1,2, or 3
	 */
	byte_count = (amlPointer[0] >> 6);
	amlPointer += (byte_count + 1);
	/* Get bytes 3, 2, 1 as needed */
	while (byte_count) {
		/*
		 * Final bit positions for the package length bytes:
		 *      Byte3->[20:27]
		 *      Byte2->[12:19]
		 *      Byte1->[04:11]
		 *      Byte0->[00:03]
		 */
		package_length |= (amlPointer[byte_count] << ((byte_count << 3) - 4));
		byte_zero_mask = 0x0F;	/* Use bits [0:3] of byte 0 */
		byte_count--;
	}
	/* Byte 0 is a special case, either bits [0:3] or [0:5] are used */
	package_length |= (amlPointer[0] & byte_zero_mask);
	return package_length;      
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
            kprint("             Device String Found: %s\n", buffer);
            
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
                uint32_t pkgLength = parsePkgLength(amlPointer);
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

// __PRIVILEGED_CODE
// char* parseAmlNamestring(uint8_t*& amlPointer) {
//     uint8_t* start = amlPointer;
// 	uint8_t* end = amlPointer;

// 	/* Point past any namestring prefix characters (backslash or carat) */
// 	while (*end == '\\' || *end == '^') {
// 		end++;
// 	}

// 	/* Decode the path prefix character */
// 	switch (*end) {
// 	case 0:
// 		/* null_name */
// 		if (end == start) {
// 			start = NULL;
// 		}
// 		end++;
// 		break;
// 	case 0x2E:
// 		/* Two name segments */
// 		end += 1 + (2 * 4);
// 		break;
// 	case 0x2F:
// 		/* Multiple name segments, 4 chars each, count in next byte */
// 		end += 2 + (*(end + 1) * 4);
// 		break;
// 	default:
// 		/* Single name segment */
// 		end += 4;
// 		break;
// 	}
// 	amlPointer = end;
// 	return (char*)start;
// }

// __PRIVILEGED_CODE
// void parseAmlTable(AcpiTableHeader* table) {
//     uint8_t* amlStart = (uint8_t*)(table + 1);
//     uint8_t* amlEnd = (uint8_t*)table + table->length;
//     uint8_t* amlPointer = amlStart;

//     while (amlPointer < amlEnd) {
//         uint8_t opcode = *amlPointer;

//         switch (opcode) {
//         case ACPI_AML_OPCODE_SCOPE: {
//             //kprint("            Opcode: ACPI_AML_OPCODE_SCOPE\n");
//             ++amlPointer;

//             uint32_t pkgLength = parsePkgLength(amlPointer);
//             uint8_t* endPtr = amlPointer + pkgLength;
//             (void)pkgLength;
//             //kprint("               pkgLength: %i\n", pkgLength);

//             char* name = parseAmlNamestring(amlPointer);

//             if (*name != '\0')
//                 kprint("                  -- namestring: %s\n", name);

//             ++amlPointer;
//             break;
//         }
//         default:
//             //kprint("            Opcode: 0x%llx\n", opcode);
//             //kprint("              next: 0x%llx\n", *(amlPointer++));
//             //return; // REMOVE LATER!!!!!!!!!!!
//             ++amlPointer;
//             break;
//         }
//     }
// }

__PRIVILEGED_CODE
void parseMcfg(McfgHeader* mcfg) {
    enumeratePciDevices(mcfg);
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
            parseDsdt(dsdt);
            //parseAmlTable(dsdt);
        } else if (memcmp(table->signature, (char*)"SSDT", 4) == 0) {
            //parseAmlTable(table);
        } else if (memcmp(table->signature, (char*)"MCFG", 4) == 0) {
            McfgHeader* mcfg = reinterpret_cast<McfgHeader*>(__va(table));
            parseMcfg(mcfg);
            break;
        }
    }
    kprint("\n");
}
