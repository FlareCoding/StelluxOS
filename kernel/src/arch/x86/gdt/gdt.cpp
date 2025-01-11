#ifdef ARCH_X86_64
#include <arch/x86/gdt/gdt.h>
#include <process/process.h>
#include <memory/memory.h>
#include <serial/serial.h>

namespace arch::x86 {
EXTERN_C
__PRIVILEGED_CODE
void asm_flush_gdt(gdt_desc* descriptor);

struct gdt_and_tss_data {
    gdt_segment_descriptor kernel_null_descriptor;
    gdt_segment_descriptor kernel_code_descriptor;
    gdt_segment_descriptor kernel_data_descriptor;
    gdt_segment_descriptor user_code_descriptor;
    gdt_segment_descriptor user_data_descriptor;
    tss_desc               tss_descriptor;

    gdt                    gdt_instance;
    task_state_segment     tss_instance;
    uint8_t                io_bitmap[0x2002];

    gdt_desc               gdt_descriptor;
} __attribute__((packed));

static_assert(sizeof(task_state_segment) == 0x68, "Unexpected TSS size");
static_assert(sizeof(gdt_and_tss_data::io_bitmap) == 0x2002, "Unexpected I/O bitmap size");

__PRIVILEGED_DATA
static gdt_and_tss_data g_gdt_per_cpu_array[MAX_SYSTEM_CPUS];

__PRIVILEGED_CODE
void set_segment_descriptor_base(
    gdt_segment_descriptor* descriptor,
    uint64_t base
) {
    descriptor->base_low  = (base & 0xffff);
    descriptor->base_mid  = (base >> 16) & 0xFF;
    descriptor->base_high = (base >> 24) & 0xFF;
}

__PRIVILEGED_CODE
void set_segment_descriptor_limit(
    gdt_segment_descriptor* descriptor,
    uint64_t limit
) {
    // Set the lower 16 bits of the limit
    descriptor->limit_low = (uint16_t)(limit & 0xFFFF);

    // Set the higher 4 bits of the limit
    descriptor->limit_high = (uint8_t)((limit >> 16) & 0xF);
}

__PRIVILEGED_CODE
void set_tss_descriptor_base(tss_desc* desc, uint64_t base) {
    desc->base_low  = (uint16_t)(base & 0xFFFF);
    desc->base_mid  = (uint8_t)((base >> 16) & 0xFF);
    desc->base_high = (uint8_t)((base >> 24) & 0xFF);
    desc->base_upper = (uint32_t)((base >> 32) & 0xFFFFFFFF);
}

__PRIVILEGED_CODE
void set_tss_descriptor_limit(tss_desc* desc, uint32_t limit) {
    desc->limit_low = (uint16_t)(limit & 0xFFFF);
    desc->limit_high = (uint8_t)((limit >> 16) & 0x0F);
}

__PRIVILEGED_CODE
void init_gdt(int cpu, uint64_t system_stack) {
    gdt_and_tss_data* data = &g_gdt_per_cpu_array[cpu];

    // Zero out all descriptors initially
    zeromem(&data->kernel_null_descriptor, sizeof(gdt_segment_descriptor));
    zeromem(&data->kernel_code_descriptor, sizeof(gdt_segment_descriptor));
    zeromem(&data->kernel_data_descriptor, sizeof(gdt_segment_descriptor));
    zeromem(&data->user_code_descriptor, sizeof(gdt_segment_descriptor));
    zeromem(&data->user_data_descriptor, sizeof(gdt_segment_descriptor));
    
    // Initialize Kernel Code Segment
    set_segment_descriptor_base(&data->kernel_code_descriptor, 0);
    set_segment_descriptor_limit(&data->kernel_code_descriptor, 0xFFFFF);
    data->kernel_code_descriptor.long_mode = 1;
    data->kernel_code_descriptor.granularity = 1;
    data->kernel_code_descriptor.available = 1;
    data->kernel_code_descriptor.access_byte.present = 1;
    data->kernel_code_descriptor.access_byte.descriptor_privilege_lvl = 0; // Kernel privilege level
    data->kernel_code_descriptor.access_byte.executable = 1; // Code segment
    data->kernel_code_descriptor.access_byte.read_write = 1;
    data->kernel_code_descriptor.access_byte.descriptor_type = 1;

    // Initialize Kernel Data Segment
    set_segment_descriptor_base(&data->kernel_data_descriptor, 0);
    set_segment_descriptor_limit(&data->kernel_data_descriptor, 0xFFFFF);
    data->kernel_data_descriptor.long_mode = 1;
    data->kernel_data_descriptor.granularity = 1;
    data->kernel_data_descriptor.available = 1;
    data->kernel_data_descriptor.access_byte.present = 1;
    data->kernel_data_descriptor.access_byte.descriptor_privilege_lvl = 0; // Kernel privilege level
    data->kernel_data_descriptor.access_byte.executable = 0; // Data segment
    data->kernel_data_descriptor.access_byte.read_write = 1;
    data->kernel_data_descriptor.access_byte.descriptor_type = 1;
    
    // Initialize user Code segment
    set_segment_descriptor_base(&data->user_code_descriptor, 0);
    set_segment_descriptor_limit(&data->user_code_descriptor, 0xFFFFF);
    data->user_code_descriptor.long_mode = 1;
    data->user_code_descriptor.granularity = 1;
    data->user_code_descriptor.available = 1;
    data->user_code_descriptor.access_byte.present = 1;
    data->user_code_descriptor.access_byte.descriptor_privilege_lvl = 3; // Usermode privilege level
    data->user_code_descriptor.access_byte.executable = 1; // Code segment
    data->user_code_descriptor.access_byte.read_write = 1;
    data->user_code_descriptor.access_byte.descriptor_type = 1;

    // Initialize user Data segment
    set_segment_descriptor_base(&data->user_data_descriptor, 0);
    set_segment_descriptor_limit(&data->user_data_descriptor, 0xFFFFF);
    data->user_data_descriptor.long_mode = 1;
    data->user_data_descriptor.granularity = 1;
    data->user_data_descriptor.available = 1;
    data->user_data_descriptor.access_byte.present = 1;
    data->user_data_descriptor.access_byte.descriptor_privilege_lvl = 3; // Usermode privilege level
    data->user_data_descriptor.access_byte.executable = 0; // Data segment
    data->user_data_descriptor.access_byte.read_write = 1;
    data->user_data_descriptor.access_byte.descriptor_type = 1;

    // Initialize tss
    zeromem(&data->tss_instance, sizeof(task_state_segment));
    data->tss_instance.rsp0 = reinterpret_cast<uint64_t>(system_stack);
    data->tss_instance.io_map_base = sizeof(task_state_segment);

    // Initialize tss descriptor
    set_tss_descriptor_base(&data->tss_descriptor, (uint64_t)&data->tss_instance);
    set_tss_descriptor_limit(&data->tss_descriptor, sizeof(task_state_segment) - 1 + sizeof(data->io_bitmap));
    data->tss_descriptor.access_byte.type = 0x9;  // 0b1001 for 64-bit TSS (Available)
    data->tss_descriptor.access_byte.present = 1;
    data->tss_descriptor.access_byte.dpl = 0; // kernel privilege level
    data->tss_descriptor.access_byte.zero = 0; // Should be zero
    data->tss_descriptor.limit_high = 0; // 64-bit TSS doesn't use limit_high, set it to 0
    data->tss_descriptor.available = 1; // If you use this field, set it to 1
    data->tss_descriptor.granularity = 0; // no granularity for TSS
    data->tss_descriptor.zero = 0; // Should be zero
    data->tss_descriptor.zero_again = 0; // should be zero

    // Initialize I/O Bitmap
    memset(&data->io_bitmap, 0xFF, sizeof(data->io_bitmap)); // Set all bits to 1 (inaccessible to userspace)

    // Allow usermode access to COM1 port by clearing the bit in the IO bitmap
    serial::mark_serial_port_unprivileged(SERIAL_PORT_BASE_COM1, cpu);

    // Ensure the end-of-bitmap marker is set to 0xFFFF
    data->io_bitmap[sizeof(data->io_bitmap) - 2] = 0xFF;
    data->io_bitmap[sizeof(data->io_bitmap) - 1] = 0xFF;

    // Update the gdt with initialized descriptors
    data->gdt_instance.kernel_null = data->kernel_null_descriptor;
    data->gdt_instance.kernel_code = data->kernel_code_descriptor;
    data->gdt_instance.kernel_data = data->kernel_data_descriptor;
    data->gdt_instance.user_code = data->user_code_descriptor;
    data->gdt_instance.user_data = data->user_data_descriptor;
    data->gdt_instance.tss = data->tss_descriptor;

    // Initialize the GDT descriptor
    data->gdt_descriptor = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)&data->gdt_instance
    };

    // Install the gdt
    asm_flush_gdt(&data->gdt_descriptor);

    // Load the Task Register (TR)
    reload_task_register();
}

__PRIVILEGED_CODE
void reload_task_register() {
    __asm__("ltr %%ax" : : "a" (__TSS_PT1_SELECTOR));
}
} // namespace arch::x86

__PRIVILEGED_CODE
void mark_port_privileged(uint16_t port) {
    // Calculate the byte and bit indices in the I/O Permission Bitmap
    size_t byte = port / 8;
    size_t bit = port % 8;

    // Iterate over all CPUs to update their respective I/O bitmaps
    for (size_t i = 0; i < MAX_SYSTEM_CPUS; i++) {
        arch::x86::g_gdt_per_cpu_array[i].io_bitmap[byte] |= (1 << bit);
    }
}

__PRIVILEGED_CODE
void mark_port_unprivileged(uint16_t port) {
    // Calculate the byte and bit indices in the I/O Permission Bitmap
    size_t byte = port / 8;
    size_t bit = port % 8;

    // Iterate over all CPUs to update their respective I/O bitmaps
    for (size_t i = 0; i < MAX_SYSTEM_CPUS; i++) {
        arch::x86::g_gdt_per_cpu_array[i].io_bitmap[byte] &= ~(1 << bit);
    }
}

__PRIVILEGED_CODE
void mark_port_privileged(uint16_t port, uint8_t cpu) {
    // Calculate the byte and bit indices in the I/O Permission Bitmap
    size_t byte = port / 8;
    size_t bit = port % 8;

    arch::x86::g_gdt_per_cpu_array[cpu].io_bitmap[byte] |= (1 << bit);
}

__PRIVILEGED_CODE
void mark_port_unprivileged(uint16_t port, uint8_t cpu) {
    // Calculate the byte and bit indices in the I/O Permission Bitmap
    size_t byte = port / 8;
    size_t bit = port % 8;

    arch::x86::g_gdt_per_cpu_array[cpu].io_bitmap[byte] &= ~(1 << bit);
}

#endif // ARCH_X86_64

