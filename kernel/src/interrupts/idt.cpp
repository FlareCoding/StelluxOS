#include "idt.h"
#include <memory/kmemory.h>
#include "panic.h"
#include <arch/x86/ioapic.h>
#include <arch/x86/apic.h>
#include <acpi/acpi_controller.h>
#include <kprint.h>

#define MAX_IRQS 64

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
EXTERN_C void __asm_irq_handler_16();
EXTERN_C void __asm_irq_handler_17();
EXTERN_C void __asm_irq_handler_18();
EXTERN_C void __asm_irq_handler_19();
EXTERN_C void __asm_irq_handler_20();
EXTERN_C void __asm_irq_handler_21();
EXTERN_C void __asm_irq_handler_22();
EXTERN_C void __asm_irq_handler_23();
EXTERN_C void __asm_irq_handler_24();
EXTERN_C void __asm_irq_handler_25();
EXTERN_C void __asm_irq_handler_26();
EXTERN_C void __asm_irq_handler_27();
EXTERN_C void __asm_irq_handler_28();
EXTERN_C void __asm_irq_handler_29();
EXTERN_C void __asm_irq_handler_30();
EXTERN_C void __asm_irq_handler_31();
EXTERN_C void __asm_irq_handler_32();
EXTERN_C void __asm_irq_handler_33();
EXTERN_C void __asm_irq_handler_34();
EXTERN_C void __asm_irq_handler_35();
EXTERN_C void __asm_irq_handler_36();
EXTERN_C void __asm_irq_handler_37();
EXTERN_C void __asm_irq_handler_38();
EXTERN_C void __asm_irq_handler_39();
EXTERN_C void __asm_irq_handler_40();
EXTERN_C void __asm_irq_handler_41();
EXTERN_C void __asm_irq_handler_42();
EXTERN_C void __asm_irq_handler_43();
EXTERN_C void __asm_irq_handler_44();
EXTERN_C void __asm_irq_handler_45();
EXTERN_C void __asm_irq_handler_46();
EXTERN_C void __asm_irq_handler_47();
EXTERN_C void __asm_irq_handler_48();
EXTERN_C void __asm_irq_handler_49();
EXTERN_C void __asm_irq_handler_50();
EXTERN_C void __asm_irq_handler_51();
EXTERN_C void __asm_irq_handler_52();
EXTERN_C void __asm_irq_handler_53();
EXTERN_C void __asm_irq_handler_54();
EXTERN_C void __asm_irq_handler_55();
EXTERN_C void __asm_irq_handler_56();
EXTERN_C void __asm_irq_handler_57();
EXTERN_C void __asm_irq_handler_58();
EXTERN_C void __asm_irq_handler_59();
EXTERN_C void __asm_irq_handler_60();
EXTERN_C void __asm_irq_handler_61();
EXTERN_C void __asm_irq_handler_62();
EXTERN_C void __asm_irq_handler_63();
EXTERN_C void __asm_irq_handler_64();

struct InterruptExceptionHandlers {
    IrqHandler_t divideByZero;
    IrqHandler_t debug;
    IrqHandler_t nmi;
    IrqHandler_t breakpoint;
    IrqHandler_t overflow;
    IrqHandler_t boundRange;
    IrqHandler_t invalidOpcode;
    IrqHandler_t deviceNotAvailable;
    IrqHandler_t doubleFault;
    IrqHandler_t coprocessorSegOverrun;
    IrqHandler_t invalidTss;
    IrqHandler_t segmentNotPresent;
    IrqHandler_t stackFault;
    IrqHandler_t generalProtectionFault;
    IrqHandler_t pageFault;
};
static_assert(sizeof(InterruptExceptionHandlers) == 15 * sizeof(IrqHandler_t));

struct IrqHandlerDescriptorTable {
    IrqDescriptor descriptors[MAX_IRQS];
};
static_assert(sizeof(IrqHandlerDescriptorTable) == MAX_IRQS * sizeof(IrqDescriptor));

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

IrqHandlerDescriptorTable g_irqHandlerTable;

IdtDescriptor g_kernelIdtDescriptor = {
    .limit = 0,
    .base = 0
};
InterruptDescriptorTable g_kernelIdt;

// Common entry point for texceptions
__PRIVILEGED_CODE
void __common_exc_entry(PtRegs* frame) {
    IrqHandler_t* handlers = (IrqHandler_t*)&g_interruptExceptionHandlers;
    if (handlers[frame->intno] != NULL) {
        irqreturn_t ret = handlers[frame->intno](frame, nullptr);
        __unused ret;
        return;
    }

    if (frame->hwframe.cs & USER_DPL) {
        // Usermode exceptions should get handled gracefully
        _userspace_common_exc_handler(frame, nullptr);
    } else {
        kpanic(frame);
    }
}

// Common entry point for IRQs
__PRIVILEGED_CODE
void __common_irq_entry(PtRegs* frame) {
    uint64_t irqIndex = frame->intno - IRQ0;

    IrqDescriptor* desc = &g_irqHandlerTable.descriptors[irqIndex];
    if (!desc->handler) {
        return;
    }

    if (desc->fastApicEoi) {
        Apic::getLocalApic()->completeIrq();
    }

    irqreturn_t ret = desc->handler(frame, desc->cookie);
    __unused ret;
}

// Common entry point for all interrupt service routines
EXTERN_C __PRIVILEGED_CODE void __common_isr_entry(PtRegs frame) {
    //if (frame.intno != IRQ0) kprint("Received int: %i\n", frame.intno);

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

    // Perform initial setup of the IRQ handler table
    zeromem(&g_irqHandlerTable, sizeof(IrqHandlerDescriptorTable));
    
    registerIrqHandler(IRQ0, _irq_handler_timer, true, nullptr);
    registerIrqHandler(IRQ16, _irq_handler_timer, true, nullptr);

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

    // Special scheduler IRQ
    SET_USER_INTERRUPT_GATE(IRQ16, __asm_irq_handler_16);
    
    SET_KERNEL_TRAP_GATE(IRQ17, __asm_irq_handler_17);
    SET_KERNEL_TRAP_GATE(IRQ18, __asm_irq_handler_18);
    SET_KERNEL_TRAP_GATE(IRQ19, __asm_irq_handler_19);
    SET_KERNEL_TRAP_GATE(IRQ20, __asm_irq_handler_20);
    SET_KERNEL_TRAP_GATE(IRQ21, __asm_irq_handler_21);
    SET_KERNEL_TRAP_GATE(IRQ22, __asm_irq_handler_22);
    SET_KERNEL_TRAP_GATE(IRQ23, __asm_irq_handler_23);
    SET_KERNEL_TRAP_GATE(IRQ24, __asm_irq_handler_24);
    SET_KERNEL_TRAP_GATE(IRQ25, __asm_irq_handler_25);
    SET_KERNEL_TRAP_GATE(IRQ26, __asm_irq_handler_26);
    SET_KERNEL_TRAP_GATE(IRQ27, __asm_irq_handler_27);
    SET_KERNEL_TRAP_GATE(IRQ28, __asm_irq_handler_28);
    SET_KERNEL_TRAP_GATE(IRQ29, __asm_irq_handler_29);
    SET_KERNEL_TRAP_GATE(IRQ30, __asm_irq_handler_30);
    SET_KERNEL_TRAP_GATE(IRQ31, __asm_irq_handler_31);
    SET_KERNEL_TRAP_GATE(IRQ32, __asm_irq_handler_32);
    SET_KERNEL_TRAP_GATE(IRQ33, __asm_irq_handler_33);
    SET_KERNEL_TRAP_GATE(IRQ34, __asm_irq_handler_34);
    SET_KERNEL_TRAP_GATE(IRQ35, __asm_irq_handler_35);
    SET_KERNEL_TRAP_GATE(IRQ36, __asm_irq_handler_36);
    SET_KERNEL_TRAP_GATE(IRQ37, __asm_irq_handler_37);
    SET_KERNEL_TRAP_GATE(IRQ38, __asm_irq_handler_38);
    SET_KERNEL_TRAP_GATE(IRQ39, __asm_irq_handler_39);
    SET_KERNEL_TRAP_GATE(IRQ40, __asm_irq_handler_40);
    SET_KERNEL_TRAP_GATE(IRQ41, __asm_irq_handler_41);
    SET_KERNEL_TRAP_GATE(IRQ42, __asm_irq_handler_42);
    SET_KERNEL_TRAP_GATE(IRQ43, __asm_irq_handler_43);
    SET_KERNEL_TRAP_GATE(IRQ44, __asm_irq_handler_44);
    SET_KERNEL_TRAP_GATE(IRQ45, __asm_irq_handler_45);
    SET_KERNEL_TRAP_GATE(IRQ46, __asm_irq_handler_46);
    SET_KERNEL_TRAP_GATE(IRQ47, __asm_irq_handler_47);
    SET_KERNEL_TRAP_GATE(IRQ48, __asm_irq_handler_48);
    SET_KERNEL_TRAP_GATE(IRQ49, __asm_irq_handler_49);
    SET_KERNEL_TRAP_GATE(IRQ50, __asm_irq_handler_50);
    SET_KERNEL_TRAP_GATE(IRQ51, __asm_irq_handler_51);
    SET_KERNEL_TRAP_GATE(IRQ52, __asm_irq_handler_52);
    SET_KERNEL_TRAP_GATE(IRQ53, __asm_irq_handler_53);
    SET_KERNEL_TRAP_GATE(IRQ54, __asm_irq_handler_54);
    SET_KERNEL_TRAP_GATE(IRQ55, __asm_irq_handler_55);
    SET_KERNEL_TRAP_GATE(IRQ56, __asm_irq_handler_56);
    SET_KERNEL_TRAP_GATE(IRQ57, __asm_irq_handler_57);
    SET_KERNEL_TRAP_GATE(IRQ58, __asm_irq_handler_58);
    SET_KERNEL_TRAP_GATE(IRQ59, __asm_irq_handler_59);
    SET_KERNEL_TRAP_GATE(IRQ60, __asm_irq_handler_60);
    SET_KERNEL_TRAP_GATE(IRQ61, __asm_irq_handler_61);
    SET_KERNEL_TRAP_GATE(IRQ62, __asm_irq_handler_62);
    SET_KERNEL_TRAP_GATE(IRQ63, __asm_irq_handler_63);
    SET_KERNEL_TRAP_GATE(IRQ64, __asm_irq_handler_64);

}

__PRIVILEGED_CODE
void loadIdtr() {
    // Load the IDT
    __asm__("lidt %0" : : "m"(g_kernelIdtDescriptor));
}

bool registerIrqHandler(uint8_t irqno, IrqHandler_t handler, bool fastApicEoi, void* cookie) {
    uint64_t irqTableIndex = static_cast<uint64_t>(irqno) - IRQ0;
    IrqDescriptor* desc = &g_irqHandlerTable.descriptors[irqTableIndex];
    if (desc->handler) {
        kuPrint("[WARN] registerIrqHandler(): IRQ%i handler already exists!\n", irqTableIndex);
        return false;
    }

    desc->handler       = handler;
    desc->fastApicEoi   = fastApicEoi;
    desc->cookie        = cookie;
    desc->irqno         = irqno;

    return true;
}

void routeIoApicIrq(uint8_t irqLine, uint8_t irqno, uint8_t cpu, uint8_t levelTriggered) {
    auto& acpiController = AcpiController::get();
    kstl::SharedPtr<IoApic>& ioapic = acpiController.getApicTable()->getIoApic(0);

    IoApic::RedirectionEntry entry;
    zeromem(&entry, sizeof(IoApic::RedirectionEntry));

    uint8_t ioapicEntryNo = irqLine;
    entry.vector = irqno;
    entry.destination = cpu;
    entry.triggerMode = levelTriggered;
    ioapic->writeRedirectionEntry(ioapicEntryNo, &entry);
}
