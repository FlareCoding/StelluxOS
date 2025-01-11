#ifdef ARCH_X86_64
#ifndef GDT_H
#define GDT_H

#include "tss.h"

/*
    Struct definitions and field descriptions taken
    from the Intel x64_IA32 Software Developer Manual.
*/

#define __KERNEL_CS         0x08
#define __KERNEL_DS         0x10
#define __TSS_PT1_SELECTOR  0x18
#define __TSS_PT2_SELECTOR  0x20
#define __USER_DS           0x28
#define __USER_CS           0x30

namespace arch::x86 {
// Structure of the GDT pointer
struct gdt_desc {
    uint16_t limit;         // Size of the GDT
    uint64_t base;          // Base address of the GDT
} __attribute__((packed));

struct gdt_segment_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    struct {
        /* Automatically set to 1 on CPU access */
        uint8_t accessed                : 1;
        
        /*
            For code segments: Readable bit.
                If clear (0), read access for this segment is not allowed.
                If set (1) read access is allowed. Write access is never allowed for code segments.

            For data segments: Writeable bit.
                If clear (0), write access for this segment is not allowed.
                If set (1) write access is allowed. Read access is always allowed for data segments.
        */
        uint8_t read_write               : 1;

        /*
            For data selectors: Direction bit.
                If clear (0) the segment grows up.
                    If set (1) the segment grows down, ie. the Offset has to be greater than the Limit.

            For code selectors: Conforming bit.
                If clear (0) code in this segment can only be executed from the ring set in DPL.
                If set (1) code in this segment can be executed from an equal or lower privilege level.
                    For example, code in ring 3 can far-jump to conforming code in a ring 2 segment.
                    The DPL field represent the highest privilege level that is allowed to execute the segment.

                    For example, code in ring 0 cannot far-jump to a conforming code segment where DPL is 2,
                    while code in ring 2 and 3 can. Note that the privilege level remains the same, ie. a
                    far-jump from ring 3 to a segment with a DPL of 2 remains in ring 3 after the jump.
        */
        uint8_t direction_conform        : 1;

        /*
            Executable bit.
                If clear (0) the descriptor defines a data segment.
                If set (1) it defines a code segment which can be executed from.
        */
        uint8_t executable              : 1;

        uint8_t descriptor_type          : 1;    // 0 = system segment   1 = code or data segment
        uint8_t descriptor_privilege_lvl  : 2;    // 0 = kernel           3 = user
        uint8_t present                 : 1;    // 0 = not present      1 = present
    } __attribute__((packed)) access_byte;
    struct {
        /*
            Specifies the size of the segment. The processor puts together the two segment limit fields to form
            a 20-bit value. The processor interprets the segment limit in one of two ways, depending on the
            setting of the G (granularity) flag:
            • If the granularity flag is clear, the segment size can range from 1 byte to 1 MByte, in byte increments.
            • If the granularity flag is set, the segment size can range from 4 KBytes to 4 GBytes, in 4-KByte
            increments.
        */
        uint8_t limit_high               : 4;

        /* Available for use by system software */
        uint8_t available               : 1;
        
        /*
            In IA-32e mode, bit 21 of the second doubleword of the segment descriptor indicates whether a
            code segment contains native 64-bit code. A value of 1 indicates instructions in this code segment
            are executed in 64-bit mode. A value of 0 indicates the instructions in this code segment are
            executed in compatibility mode. If L-bit is set, then D-bit must be cleared. When not in IA-32e mode
            or for non-code segments, bit 21 is reserved and should always be set to 0
        */
        uint8_t long_mode                : 1;

        /*
            Performs different functions depending on whether the segment descriptor is an executable code
            segment, an expand-down data segment, or a stack segment. (This flag should always be set to 1
            for 32-bit code and data segments and to 0 for 16-bit code and data segments.)
                • Executable code segment. The flag is called the D flag and it indicates the default length for
                effective addresses and operands referenced by instructions in the segment. If the flag is set,
                32-bit addresses and 32-bit or 8-bit operands are assumed; if it is clear, 16-bit addresses and
                16-bit or 8-bit operands are assumed.
                The instruction prefix 66H can be used to select an operand size other than the default, and the
                prefix 67H can be used select an address size other than the default.
                • Stack segment (data segment pointed to by the SS register). The flag is called the B (big)
                flag and it specifies the size of the stack pointer used for implicit stack operations (such as
                pushes, pops, and calls). If the flag is set, a 32-bit stack pointer is used, which is stored in the
                32-bit ESP register; if the flag is clear, a 16-bit stack pointer is used, which is stored in the 16-
                bit SP register. If the stack segment is set up to be an expand-down data segment (described in
                the next paragraph), the B flag also specifies the upper bound of the stack segment.
                • Expand-down data segment. The flag is called the B flag and it specifies the upper bound of
                the segment. If the flag is set, the upper bound is FFFFFFFFH (4 GBytes); if the flag is clear, the
                upper bound is FFFFH (64 KBytes).
        */
        uint8_t default_bound            : 1;

        /*
            Determines the scaling of the segment limit field. When the granularity flag is clear, the segment
            limit is interpreted in byte units; when flag is set, the segment limit is interpreted in 4-KByte units.
            (This flag does not affect the granularity of the base address; it is always byte granular.) When the
            granularity flag is set, the twelve least significant bits of an offset are not tested when checking the
            offset against the segment limit. For example, when the granularity flag is set, a limit of 0 results in
            valid offsets from 0 to 4095.
        */
        uint8_t granularity             : 1;
    } __attribute__((packed));
    uint8_t base_high;
} __attribute__((packed));

struct gdt {
    gdt_segment_descriptor kernel_null;    // 0x00
    gdt_segment_descriptor kernel_code;    // 0x08
    gdt_segment_descriptor kernel_data;    // 0x10
    tss_desc               tss;            // 0x18
    gdt_segment_descriptor user_data;      // 0x28
    gdt_segment_descriptor user_code;      // 0x30
};

/**
 * @brief Sets the base address for a GDT segment descriptor.
 * @param descriptor Pointer to the GDT segment descriptor to modify.
 * @param base The base address to set for the segment.
 * 
 * This function configures the base address field of the specified GDT segment descriptor.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_segment_descriptor_base(
    gdt_segment_descriptor* descriptor,
    uint64_t base
);

/**
 * @brief Sets the limit for a GDT segment descriptor.
 * @param descriptor Pointer to the GDT segment descriptor to modify.
 * @param limit The limit to set for the segment.
 * 
 * This function configures the limit field of the specified GDT segment descriptor. The limit determines
 * the maximum addressable range of the segment.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_segment_descriptor_limit(
    gdt_segment_descriptor* descriptor,
    uint64_t limit
);

/**
 * @brief Initializes the Global Descriptor Table (GDT) for a specific CPU.
 * @param cpu The CPU ID for which to initialize the GDT.
 * @param system_stack The address of the system stack to associate with the CPU.
 * 
 * Sets up the GDT for the specified CPU, including configuring the system stack and necessary
 * segment descriptors.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_gdt(int cpu, uint64_t system_stack);

/**
 * @brief Reloads the Task Register (TR) for the current CPU.
 * 
 * This function reloads the Task Register to ensure the CPU uses the updated Task-State Segment (TSS)
 * descriptor from the GDT. It is typically invoked after initializing or modifying the GDT for a CPU.
 * 
 * @note This operation is critical for correct system behavior, particularly for handling interrupts and 
 * switching tasks in protected or long mode.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void reload_task_register();
} // namespace arch::x86

#endif // GDT_H
#endif // ARCH_X86_64
