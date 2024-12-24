#ifdef ARCH_X86_64
#ifndef IOAPIC_H
#define IOAPIC_H
#include <memory/memory.h>

#define IOAPICID          0x00
#define IOAPICVER         0x01
#define IOAPICARB         0x02
#define IOAPICREDTBL(n)   (0x10 + 2 * n) // lower-32bits (add +1 for upper 32-bits)

#define IOAPIC_REGSEL     0x00
#define IOAPIC_IOWIN      0x10

namespace arch::x86 {
class ioapic {
public:
    enum delivery_mode {
        edge  = 0,
        level = 1,
    };

    enum destination_mode {
        physical = 0,
        logical  = 1
    };

    union redirection_entry {
        struct {
            uint64_t vector        : 8;
            uint64_t delv_mode     : 3;
            uint64_t dest_mode     : 1;
            uint64_t delv_status   : 1;
            uint64_t pin_polarity  : 1;
            uint64_t remote_irr    : 1;
            uint64_t trigger_mode  : 1;
            uint64_t mask          : 1;
            uint64_t reserved      : 39;
            uint64_t destination   : 8;
        };

        struct {
            uint32_t lower_dword;
            uint32_t upper_dword;
        };
    };

    __PRIVILEGED_CODE
    static kstl::shared_ptr<ioapic>& get();

    __PRIVILEGED_CODE
    static void create(uint64_t physbase, uint64_t gsib);

    // Getter functions
    uint8_t get_id() const { return m_apic_id; }
    uint8_t get_version() const { return m_apic_version; }
    uint8_t get_redirection_entry_count() const { return m_redirection_entry_count; }
    uint64_t get_global_interrupt_base() const { return m_global_intr_base; }

    __PRIVILEGED_CODE
    ioapic(uint64_t phys_regs, uint64_t gsib);

    /*
     * @param ent_no - entry number for which redirection entry is required
     * @return redirection entry associated with entry number
     */
    redirection_entry get_redirection_entry(uint8_t ent_no) const;

    /*
     * @param ent_no - entry number for which redirection entry is required
     * @param entry - pointer to entry to write
     * @return true if write was successful, false otherwise
     */
    bool write_redirection_entry(uint8_t ent_no, const redirection_entry *entry);

private:
    /*
     * This field contains the physical base address for the IOAPIC
     * which can be found using an IOAPIC entry in the ACPI 2.0 MADT.
     */
    uintptr_t m_physical_base;

    /*
     * Holds the base address of the registers in virtual memory. This
     * address is non-cacheable (see paging).
     */
    uintptr_t m_virtual_base;

    /*
     * Software has complete control over the APIC ID. Also, hardware
     * won't automatically change its APIC ID, so we could cache it here.
     */
    uint8_t m_apic_id;

    /*
     * Hardware version of the APIC, mainly for display purposes.
     */
    uint8_t m_apic_version;

    /*
     * Although entries for the current IOAPIC are 24, it may change. To retain
     * compatibility, make sure you use this.
     */
    uint8_t m_redirection_entry_count;

    /*
     * The first IRQ which this IOAPIC handles. This is only found in the
     * IOAPIC entry of the ACPI 2.0 MADT. It isn't found in the IOAPIC
     * registers.
     */
    uint64_t m_global_intr_base;

    /*
     * Reads the data present in the register at offset reg_off.
     *
     * @param reg_off - the register's offset which is being read
     * @return the data present in the register associated with that offset
     */
    uint32_t read(uint8_t reg_off) const;

    /*
     * Writes the data into the register associated.
     *
     * @param reg_off - the register's offset which is being written
     * @param data - dword to write to the register
     */
    void write(uint8_t reg_off, uint32_t data);
};
} // namespace arch::x86

#endif // IOAPIC_H
#endif // ARCH_X86_64
