#include "idt.h"
#include <memory/kmemory.h>
#include "panic.h"
#include <arch/x86/ioapic.h>
#include <acpi/acpi_controller.h>
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

struct InterruptExceptionHandlers {
    InterruptHandler_t divideByZero;
    InterruptHandler_t debug;
    InterruptHandler_t nmi;
    InterruptHandler_t breakpoint;
    InterruptHandler_t overflow;
    InterruptHandler_t boundRange;
    InterruptHandler_t invalidOpcode;
    InterruptHandler_t deviceNotAvailable;
    InterruptHandler_t doubleFault;
    InterruptHandler_t coprocessorSegOverrun;
    InterruptHandler_t invalidTss;
    InterruptHandler_t segmentNotPresent;
    InterruptHandler_t stackFault;
    InterruptHandler_t generalProtectionFault;
    InterruptHandler_t pageFault;
};
static_assert(sizeof(InterruptExceptionHandlers) == 15 * sizeof(InterruptHandler_t));

struct InterruptIrqHandlers {
    InterruptHandler_t timerIrq;
    InterruptHandler_t irq1;
    InterruptHandler_t irq2;
    InterruptHandler_t irq3;
    InterruptHandler_t irq4;
    InterruptHandler_t irq5;
    InterruptHandler_t irq6;
    InterruptHandler_t irq7;
    InterruptHandler_t irq8;
    InterruptHandler_t irq9;
    InterruptHandler_t irq10;
    InterruptHandler_t irq11;
    InterruptHandler_t irq12;
    InterruptHandler_t irq13;
    InterruptHandler_t irq14;
    InterruptHandler_t irq15;
};
static_assert(sizeof(InterruptIrqHandlers) == 16 * sizeof(InterruptHandler_t));

InterruptExceptionHandlers g_interruptExceptionHandlers = {
    .divideByZero = _exc_handler_div,
    .debug = 0,
    .nmi = 0,
    .breakpoint = 0,
    .overflow = 0,
    .boundRange = 0,
    .invalidOpcode = 0,
    .deviceNotAvailable = 0,
    .doubleFault = 0,
    .coprocessorSegOverrun = 0,
    .invalidTss = 0,
    .segmentNotPresent = 0,
    .stackFault = 0,
    .generalProtectionFault = 0,
    .pageFault = _exc_handler_pf
};

InterruptIrqHandlers g_interruptIrqHandlers = {
    .timerIrq = _irq_handler_timer,
    .irq1 = 0,
    .irq2 = 0,
    .irq3 = 0,
    .irq4 = 0,
    .irq5 = 0,
    .irq6 = 0,
    .irq7 = 0,
    .irq8 = 0,
    .irq9 = 0,
    .irq10 = 0,
    .irq11 = 0,
    .irq12 = 0,
    .irq13 = 0,
    .irq14 = 0,
    .irq15 = _irq_handler_schedule,
};

IdtDescriptor g_kernelIdtDescriptor = {
    .limit = 0,
    .base = 0
};
InterruptDescriptorTable g_kernelIdt;

// Common entry point for texceptions
__PRIVILEGED_CODE
void __common_exc_entry(PtRegs* frame) {
    InterruptHandler_t* handlers = (InterruptHandler_t*)&g_interruptExceptionHandlers;
    if (handlers[frame->intno] != NULL) {
        return handlers[frame->intno](frame);
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
    InterruptHandler_t* handlers = (InterruptHandler_t*)&g_interruptIrqHandlers;
    if (handlers[frame->intno - IRQ0] != NULL) {
        handlers[frame->intno - IRQ0](frame);
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

    // Set Userspace IRQ Handlers
    SET_USER_INTERRUPT_GATE(IRQ15, __asm_irq_handler_15);
}

__PRIVILEGED_CODE
void loadIdtr() {
    // Load the IDT
    __asm__("lidt %0" : : "m"(g_kernelIdtDescriptor));
}

bool registerIrqHandler(InterruptHandler_t handler, uint8_t intrLine, uint8_t irqVector, uint8_t levelTriggered, uint8_t cpu) {
    uint8_t irqArrayIdx = irqVector - IRQ0;
    InterruptHandler_t* handlers = (InterruptHandler_t*)&g_interruptIrqHandlers;
    if (handlers[irqArrayIdx] != nullptr) {
        kuPrint("[WARN] registerIrqHandler(): IRQ%i handler already exists!\n", irqArrayIdx);
        return false;
    }

    // Set the handler pointer
    handlers[irqArrayIdx] = handler;

    auto& acpiController = AcpiController::get();
    kstl::SharedPtr<IoApic>& ioapic = acpiController.getApicTable()->getIoApic(0);

    IoApic::RedirectionEntry entry;
    zeromem(&entry, sizeof(IoApic::RedirectionEntry));

    uint8_t ioapicEntryNo = intrLine;
    entry.vector = irqVector;
    entry.destination = cpu;
    entry.triggerMode = levelTriggered;
    ioapic->writeRedirectionEntry(ioapicEntryNo, &entry);

    return true;
}
