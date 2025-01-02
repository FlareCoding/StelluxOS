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
/**
 * @class ioapic
 * @brief Manages the Input/Output Advanced Programmable Interrupt Controller (I/O APIC).
 * 
 * This class provides functionality for initializing, configuring, and controlling the I/O APIC, which handles
 * interrupt redirection and delivery for external hardware interrupts.
 */
class ioapic {
public:
    /**
     * @enum delivery_mode
     * @brief Specifies the interrupt delivery mode.
     */
    enum delivery_mode {
        edge  = 0,
        level = 1,
    };

    /**
     * @enum destination_mode
     * @brief Specifies the destination mode for interrupts.
     */
    enum destination_mode {
        physical = 0,
        logical  = 1
    };

    /**
     * @union redirection_entry
     * @brief Represents a redirection entry in the I/O APIC.
     * 
     * This union provides access to the redirection entry as either a bitfield or two 32-bit words.
     */
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

    /**
     * @brief Retrieves the singleton instance of the I/O APIC.
     * @return A shared pointer to the singleton ioapic instance.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static kstl::shared_ptr<ioapic>& get();

    /**
     * @brief Creates and initializes a new I/O APIC instance.
     * @param physbase The physical base address of the I/O APIC registers.
     * @param gsib The global system interrupt base for this I/O APIC.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void create(uint64_t physbase, uint64_t gsib);

    /**
     * @brief Retrieves the APIC ID of the I/O APIC.
     * @return The APIC ID.
     */
    uint8_t get_id() const { return m_apic_id; }

    /**
     * @brief Retrieves the APIC version of the I/O APIC.
     * @return The APIC version.
     */
    uint8_t get_version() const { return m_apic_version; }

    /**
     * @brief Retrieves the number of redirection entries supported by the I/O APIC.
     * @return The redirection entry count.
     */
    uint8_t get_redirection_entry_count() const { return m_redirection_entry_count; }

    /**
     * @brief Retrieves the global interrupt base for this I/O APIC.
     * @return The global interrupt base.
     */
    uint64_t get_global_interrupt_base() const { return m_global_intr_base; }

    /**
     * @brief Constructs and initializes an I/O APIC instance.
     * @param phys_regs The physical address of the I/O APIC registers.
     * @param gsib The global system interrupt base.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE ioapic(uint64_t phys_regs, uint64_t gsib);

    /**
     * @brief Retrieves a redirection entry from the I/O APIC.
     * @param ent_no The entry number to retrieve.
     * @return The redirection entry.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE redirection_entry get_redirection_entry(uint8_t ent_no) const;

    /**
     * @brief Writes a redirection entry to the I/O APIC.
     * @param ent_no The entry number to write to.
     * @param entry A pointer to the redirection entry to write.
     * @return True if the operation was successful, false otherwise.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool write_redirection_entry(uint8_t ent_no, const redirection_entry *entry);

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

    /**
     * @brief Reads a register from the I/O APIC.
     * @param reg_off The register offset to read from.
     * @return The value of the register.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint32_t _read(uint8_t reg_off) const;

    /**
     * @brief Writes a value to a register in the I/O APIC.
     * @param reg_off The register offset to write to.
     * @param data The value to write to the register.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void _write(uint8_t reg_off, uint32_t data);
};
} // namespace arch::x86

#endif // IOAPIC_H
#endif // ARCH_X86_64
