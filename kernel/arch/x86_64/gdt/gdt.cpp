#include "gdt/gdt.h"
#include "defs/segments.h"
#include "percpu/percpu.h"

static constexpr uint64_t IST_STACK_SIZE = 0x1000;
static constexpr uint64_t BOOT_KERNEL_STACK_SIZE = 0x4000;

__PRIVILEGED_BSS alignas(16) static uint8_t g_bsp_ist1_stack[IST_STACK_SIZE];
__PRIVILEGED_BSS alignas(16) static uint8_t g_bsp_ist2_stack[IST_STACK_SIZE];
__PRIVILEGED_BSS alignas(16) static uint8_t g_bsp_ist3_stack[IST_STACK_SIZE];
__PRIVILEGED_BSS alignas(16) static uint8_t g_bsp_kernel_stack[BOOT_KERNEL_STACK_SIZE];

DEFINE_PER_CPU(x86::gdt_entry[x86::GDT_ENTRIES], cpu_gdt);
DEFINE_PER_CPU(x86::tss, cpu_tss);
DEFINE_PER_CPU(x86::gdt_ptr, cpu_gdt_ptr);
DEFINE_PER_CPU(x86::system_segment_descriptor, cpu_tss_descriptor);

extern "C" void stlx_x86_64_gdt_flush(const x86::gdt_ptr* gdtr, uint16_t cs, uint16_t ds, uint16_t tss);

namespace x86 {
namespace gdt {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(uintptr_t rsp0, uintptr_t ist1, uintptr_t ist2, uintptr_t ist3) {
    auto& gdt = this_cpu(cpu_gdt);
    auto& tss_ref = this_cpu(cpu_tss);
    auto& gdt_ptr_ref = this_cpu(cpu_gdt_ptr);
    auto& tss_desc = this_cpu(cpu_tss_descriptor);

    gdt[GDT_NULL_IDX].segment = make_null_descriptor();

    gdt[GDT_KERNEL_CS_IDX].segment = make_segment_descriptor(
        access::KERNEL_CODE,
        flags::CODE_64
    );

    gdt[GDT_KERNEL_DS_IDX].segment = make_segment_descriptor(
        access::KERNEL_DATA,
        flags::DATA_64
    );

    gdt[GDT_USER_DS_IDX].segment = make_segment_descriptor(
        access::USER_DATA,
        flags::DATA_64
    );

    gdt[GDT_USER_CS_IDX].segment = make_segment_descriptor(
        access::USER_CODE,
        flags::CODE_64
    );

    uint8_t* tss_bytes = reinterpret_cast<uint8_t*>(&tss_ref);
    for (uint64_t i = 0; i < sizeof(tss); i++) {
        tss_bytes[i] = 0;
    }

    tss_ref.rsp0 = rsp0;
    tss_ref.ist1 = ist1;
    tss_ref.ist2 = ist2;
    tss_ref.ist3 = ist3;
    tss_ref.iopb_offset = sizeof(tss);

    uint64_t tss_base = reinterpret_cast<uint64_t>(&tss_ref);
    uint32_t tss_limit = sizeof(tss) - 1;

    tss_desc = make_system_descriptor(
        tss_base,
        tss_limit,
        system_type::TSS_AVAILABLE
    );

    uint8_t* tss_dst = reinterpret_cast<uint8_t*>(&gdt[GDT_TSS_IDX]);
    const uint8_t* tss_src = reinterpret_cast<const uint8_t*>(&tss_desc);
    for (uint64_t i = 0; i < sizeof(system_segment_descriptor); i++) {
        tss_dst[i] = tss_src[i];
    }

    gdt_ptr_ref.limit = sizeof(gdt) - 1;
    gdt_ptr_ref.base = reinterpret_cast<uint64_t>(&gdt[0]);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_bsp() {
    return init(
        reinterpret_cast<uintptr_t>(&g_bsp_kernel_stack[BOOT_KERNEL_STACK_SIZE]),
        reinterpret_cast<uintptr_t>(&g_bsp_ist1_stack[IST_STACK_SIZE]),
        reinterpret_cast<uintptr_t>(&g_bsp_ist2_stack[IST_STACK_SIZE]),
        reinterpret_cast<uintptr_t>(&g_bsp_ist3_stack[IST_STACK_SIZE])
    );
}

__PRIVILEGED_CODE uintptr_t get_bsp_kernel_stack_top() {
    return reinterpret_cast<uintptr_t>(&g_bsp_kernel_stack[BOOT_KERNEL_STACK_SIZE]);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void load() {
    stlx_x86_64_gdt_flush(&this_cpu(cpu_gdt_ptr), KERNEL_CS, KERNEL_DS, TSS_SEL);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_rsp0(uintptr_t rsp0) {
    this_cpu(cpu_tss).rsp0 = rsp0;
}

} // namespace gdt
} // namespace x86
