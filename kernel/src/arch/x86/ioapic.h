#ifndef IOAPIC_H
#define IOAPIC_H
#include <ktypes.h>

#define IOAPICID          0x00
#define IOAPICVER         0x01
#define IOAPICARB         0x02
#define IOAPICREDTBL(n)   (0x10 + 2 * n) // lower-32bits (add +1 for upper 32-bits)

#define IOAPIC_REGSEL 0x00
#define IOAPIC_IOWIN  0x10

/* @class IOAPIC
 *
 * A sample driver code which control an IOAPIC. It handles one IOAPIC and exposes
 * some functions. It is totally representational, .i.e you should add locking support
 * link all IOAPIC classes in a data structure and much more. Here we are just showing
 * what & how your'e gonna handle this in C++.
 *
 * You could also note that IOAPIC registers "may" cross a page boundary. So maybe you
 * may need to map the physical-base to a double-page (means allocate twice the amount
 * of memory from vmm).
 */
class IoApic {
public:
    enum DeliveryMode {
            EDGE  = 0,
            LEVEL = 1,
    };

    enum DestinationMode {
            PHYSICAL = 0,
            LOGICAL  = 1
    };

    union RedirectionEntry {
        struct {
            uint64_t vector       : 8;
            uint64_t delvMode     : 3;
            uint64_t destMode     : 1;
            uint64_t delvStatus   : 1;
            uint64_t pinPolarity  : 1;
            uint64_t remoteIRR    : 1;
            uint64_t triggerMode  : 1;
            uint64_t mask         : 1;
            uint64_t reserved     : 39;
            uint64_t destination  : 8;
        };
                
        struct {
            uint32_t lowerDword;
            uint32_t upperDword;
        };
    };

    uint8_t getId() { return m_apicId; }
    uint8_t getVersion() { return m_apicVersion; }
    uint8_t getRedirectionEntryCount() { return m_redirectionEntryCount; }
    uint64_t getGlobalInterruptBase() { return m_globalIntrBase; }

    IoApic(uint64_t physRegs, uint64_t gsib);

    /*
    * @param entNo - entry no. for which redir-entry is required
    * @return entry associated with entry no.
    */
    RedirectionEntry getRedirectionEntry(uint8_t entNo);

    /*
    * @param entNo - entry no. for which redir-entry is required
    * @param entry - ptr to entry to write
    */
    bool writeRedirectionEntry(uint8_t entNo, RedirectionEntry *entry);

private:
    /*
    * This field contains the physical-base address for the IOAPIC
    * can be found using an IOAPIC-entry in the ACPI 2.0 MADT.
    */
    uint64_t m_physicalBase;

    /*
    * Holds the base address of the registers in virtual memory. This
    * address is non-cacheable (see paging).
    */
    uint64_t m_virtualBase;

    /*
    * Software has complete control over the apic-id. Also, hardware
    * won't automatically change its apic-id so we could cache it here.
    */
    uint8_t m_apicId;

    /*
    * Hardware-version of the apic, mainly for display purpose.
    */
    uint8_t m_apicVersion;

    /*
    * Although entries for current IOAPIC is 24, it may change. To retain
    * compatibility make sure you use this.
    */
    uint8_t m_redirectionEntryCount;

    /*
    * The first IRQ which this IOAPIC handles. This is only found in the
    * IOAPIC entry of the ACPI 2.0 MADT. It isn't found in the IOAPIC
    * registers.
    */
    uint64_t m_globalIntrBase;

    /*
    * Reads the data present in the register at offset regOff.
    *
    * @param regOff - the register's offset which is being read
    * @return the data present in the register associated with that offset
    */
    uint32_t read(uint8_t regOff);

    /*
    * Writes the data into the register associated. 
    *
    * @param regOff - the register's offset which is being written
    * @param data - dword to write to the register
    */
    void write(uint8_t regOff, uint32_t data);
};

#endif
