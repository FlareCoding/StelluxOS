#include "idt.h"
#include <memory/kmemory.h>
#include "panic.h"
#include <kprint.h>

EXTERN_C void __asm_exc_handler_div();
EXTERN_C void __asm_exc_handler_db();
EXTERN_C void __asm_exc_handler_nmi();
EXTERN_C void __asm_exc_handler_bp();
EXTERN_C void __asm_exc_handler_of();
EXTERN_C void __asm_exc_handler_br();
EXTERN_C void __asm_exc_handler_ud();
EXTERN_C void __asm_exc_handler_nm();
EXTERN_C void __asm_exc_handler_df();
EXTERN_C void __asm_exc_handler_cso();
EXTERN_C void __asm_exc_handler_ts();
EXTERN_C void __asm_exc_handler_np();
EXTERN_C void __asm_exc_handler_ss();
EXTERN_C void __asm_exc_handler_gp();
EXTERN_C void __asm_exc_handler_pf();
EXTERN_C void __asm_exc_handler_mf();
EXTERN_C void __asm_exc_handler_ac();
EXTERN_C void __asm_exc_handler_mc();
EXTERN_C void __asm_exc_handler_xm();
EXTERN_C void __asm_exc_handler_ve();
EXTERN_C void __asm_exc_handler_cp();
EXTERN_C void __asm_exc_handler_hv();
EXTERN_C void __asm_exc_handler_vc();
EXTERN_C void __asm_exc_handler_sx();

EXTERN_C void __asm_irq_handler_0();
EXTERN_C void __asm_irq_handler_1();
EXTERN_C void __asm_irq_handler_2();
EXTERN_C void __asm_irq_handler_3();
EXTERN_C void __asm_irq_handler_4();
EXTERN_C void __asm_irq_handler_5();
EXTERN_C void __asm_irq_handler_6();
EXTERN_C void __asm_irq_handler_7();
EXTERN_C void __asm_irq_handler_8();
EXTERN_C void __asm_irq_handler_9();
EXTERN_C void __asm_irq_handler_10();
EXTERN_C void __asm_irq_handler_11();
EXTERN_C void __asm_irq_handler_12();
EXTERN_C void __asm_irq_handler_13();
EXTERN_C void __asm_irq_handler_14();
EXTERN_C void __asm_irq_handler_15();

InterruptHandler_t g_int_exc_handlers[15] = {
    _exc_handler_div,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    _exc_handler_pf
};

InterruptHandler_t g_int_irq_handlers[15] = {
    _irq_handler_timer,
    _irq_handler_keyboard,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

IdtDescriptor g_kernelIdtDescriptor = {
    .limit = 0,
    .base = 0
};
InterruptDescriptorTable g_kernelIdt;

// Common entry point for texceptions
__PRIVILEGED_CODE
void __common_exc_entry(PtRegs* frame) {
    if (g_int_exc_handlers[frame->intno] != NULL) {
        return g_int_exc_handlers[frame->intno](frame);
    }

    if (frame->hwframe.cs & USER_DPL) {
        // Usermode exceptions should get handled gracefully
        _userspace_common_exc_handler(frame);
    } else {
        kpanic(frame);
    }
}

// Common entry point for IRQs
__PRIVILEGED_CODE
void __common_irq_entry(PtRegs* frame) {
    if (g_int_irq_handlers[frame->intno - IRQ0] != NULL) {
        g_int_irq_handlers[frame->intno - IRQ0](frame);
    }
}

// Common entry point for all interrupt service routines
EXTERN_C __PRIVILEGED_CODE void __common_isr_entry(PtRegs frame) {
    //kprint("Received int: %i\n", frame.intno);

    // Check whether the interrupt is an IRQ or a trap/exception
    if (frame.intno >= IRQ0) {
        __common_irq_entry(&frame);
    } else {
        __common_exc_entry(&frame);
    }
}

void setupInterruptDescriptorTable() {
    g_kernelIdtDescriptor.limit = sizeof(g_kernelIdt) - 1;
    g_kernelIdtDescriptor.base = reinterpret_cast<uint64_t>(&g_kernelIdt);

    // Set exception handlers
    SET_KERNEL_INTERRUPT_GATE(EXC_DIVIDE_BY_ZERO,           __asm_exc_handler_div);
    SET_KERNEL_INTERRUPT_GATE(EXC_DEBUG,                    __asm_exc_handler_db);
    SET_KERNEL_INTERRUPT_GATE(EXC_NMI,                      __asm_exc_handler_nmi);
    SET_KERNEL_INTERRUPT_GATE(EXC_BREAKPOINT,               __asm_exc_handler_bp);
    SET_KERNEL_INTERRUPT_GATE(EXC_OVERFLOW,                 __asm_exc_handler_of);
    SET_KERNEL_INTERRUPT_GATE(EXC_BOUND_RANGE,              __asm_exc_handler_br);
    SET_KERNEL_INTERRUPT_GATE(EXC_INVALID_OPCODE,           __asm_exc_handler_ud);
    SET_KERNEL_INTERRUPT_GATE(EXC_DEVICE_NOT_AVAILABLE,     __asm_exc_handler_nm);
    SET_KERNEL_INTERRUPT_GATE(EXC_DOUBLE_FAULT,             __asm_exc_handler_df);
    SET_KERNEL_INTERRUPT_GATE(EXC_COPROCESSOR_SEG_OVERRUN,  __asm_exc_handler_cso);
    SET_KERNEL_INTERRUPT_GATE(EXC_INVALID_TSS,              __asm_exc_handler_ts);
    SET_KERNEL_INTERRUPT_GATE(EXC_SEGMENT_NOT_PRESENT,      __asm_exc_handler_np);
    SET_KERNEL_INTERRUPT_GATE(EXC_STACK_FAULT,              __asm_exc_handler_ss);
    SET_KERNEL_INTERRUPT_GATE(EXC_GENERAL_PROTECTION,       __asm_exc_handler_gp);
    SET_KERNEL_INTERRUPT_GATE(EXC_PAGE_FAULT,               __asm_exc_handler_pf);
    SET_KERNEL_INTERRUPT_GATE(EXC_X87_FLOATING_POINT,       __asm_exc_handler_mf);
    SET_KERNEL_INTERRUPT_GATE(EXC_ALIGNMENT_CHECK,          __asm_exc_handler_ac);
    SET_KERNEL_INTERRUPT_GATE(EXC_MACHINE_CHECK,            __asm_exc_handler_mc);
    SET_KERNEL_INTERRUPT_GATE(EXC_SIMD_FLOATING_POINT,      __asm_exc_handler_xm);
    SET_KERNEL_INTERRUPT_GATE(EXC_VIRTUALIZATION,           __asm_exc_handler_ve);
    SET_KERNEL_INTERRUPT_GATE(EXC_SECURITY_EXCEPTION,       __asm_exc_handler_cp);
    SET_KERNEL_INTERRUPT_GATE(EXC_HYPERVISOR_VIOLATION,     __asm_exc_handler_hv);
    SET_KERNEL_INTERRUPT_GATE(EXC_VMM_COMMUNICATION,        __asm_exc_handler_vc);
    SET_KERNEL_INTERRUPT_GATE(EXC_SECURITY_EXTENSION,       __asm_exc_handler_sx);

    // Set IRQ Handlers
    SET_KERNEL_TRAP_GATE(IRQ0, __asm_irq_handler_0);
    SET_KERNEL_TRAP_GATE(IRQ1, __asm_irq_handler_1);
    SET_KERNEL_TRAP_GATE(IRQ2, __asm_irq_handler_2);
    SET_KERNEL_TRAP_GATE(IRQ3, __asm_irq_handler_3);
    SET_KERNEL_TRAP_GATE(IRQ4, __asm_irq_handler_4);
    SET_KERNEL_TRAP_GATE(IRQ5, __asm_irq_handler_5);
    SET_KERNEL_TRAP_GATE(IRQ6, __asm_irq_handler_6);
    SET_KERNEL_TRAP_GATE(IRQ7, __asm_irq_handler_7);
    SET_KERNEL_TRAP_GATE(IRQ8, __asm_irq_handler_8);
    SET_KERNEL_TRAP_GATE(IRQ9, __asm_irq_handler_9);
    SET_KERNEL_TRAP_GATE(IRQ10, __asm_irq_handler_10);
    SET_KERNEL_TRAP_GATE(IRQ11, __asm_irq_handler_11);
    SET_KERNEL_TRAP_GATE(IRQ12, __asm_irq_handler_12);
    SET_KERNEL_TRAP_GATE(IRQ13, __asm_irq_handler_13);
    SET_KERNEL_TRAP_GATE(IRQ14, __asm_irq_handler_14);
    SET_KERNEL_TRAP_GATE(IRQ15, __asm_irq_handler_15);
}

__PRIVILEGED_CODE
void loadIdtr() {
    // Load the IDT
    __asm__("lidt %0" : : "m"(g_kernelIdtDescriptor));
}
