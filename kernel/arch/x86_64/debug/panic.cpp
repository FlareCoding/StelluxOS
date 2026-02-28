#include "debug/panic.h"
#include "defs/vectors.h"
#include "trap/trap_frame.h"
#include "debug/symtab.h"
#include "debug/dwarf_line.h"
#include "debug/stacktrace.h"
#include "common/logging.h"
#include "hw/cpu.h"
#include "sched/sched.h"
#include "sched/task.h"

namespace panic {

static const char* exception_name(uint64_t vec) {
    switch (vec) {
        case 0x00: return "Divide Error (#DE)";
        case 0x01: return "Debug (#DB)";
        case 0x02: return "NMI";
        case 0x03: return "Breakpoint (#BP)";
        case 0x04: return "Overflow (#OF)";
        case 0x05: return "Bound Range Exceeded (#BR)";
        case 0x06: return "Invalid Opcode (#UD)";
        case 0x07: return "Device Not Available (#NM)";
        case 0x08: return "Double Fault (#DF)";
        case 0x0A: return "Invalid TSS (#TS)";
        case 0x0B: return "Segment Not Present (#NP)";
        case 0x0C: return "Stack-Segment Fault (#SS)";
        case 0x0D: return "General Protection Fault (#GP)";
        case 0x0E: return "Page Fault (#PF)";
        case 0x10: return "x87 FPU Error (#MF)";
        case 0x11: return "Alignment Check (#AC)";
        case 0x12: return "Machine Check (#MC)";
        case 0x13: return "SIMD Floating-Point (#XM)";
        case 0x14: return "Virtualization (#VE)";
        case 0x15: return "Control Protection (#CP)";
        case 0x1C: return "Hypervisor Injection (#HV)";
        case 0x1D: return "VMM Communication (#VC)";
        case 0x1E: return "Security Exception (#SX)";
        default:   return "Unknown Exception";
    }
}

static void print_pf_details(uint64_t error_code, uint64_t cr2) {
    log::panic_write("  Faulting address (CR2): 0x%016lx", cr2);

    const char* present = (error_code & 0x1) ? "protection-violation" : "not-present";
    const char* rw      = (error_code & 0x2) ? "write" : "read";
    const char* mode    = (error_code & 0x4) ? "user" : "supervisor";

    log::panic_write("  Error code: 0x%04lx (%s | %s | %s%s%s)",
        error_code, present, rw, mode,
        (error_code & 0x8)  ? " | reserved-bit-set" : "",
        (error_code & 0x10) ? " | instruction-fetch" : "");

    if (cr2 < 0x1000) {
        if (cr2 == 0) {
            log::panic_write("  -> Null pointer dereference");
        } else {
            log::panic_write("  -> Null pointer dereference (%s at offset 0x%lx from null)", rw, cr2);
        }
    }
}

static void print_frame(int index, uint64_t addr) {
    symtab::resolve_result sym;
    dwarf_line::resolve_result loc;
    bool has_sym  = symtab::resolve(addr, &sym);
    bool has_line = dwarf_line::resolve(addr, &loc);

    if (has_sym && has_line) {
        log::panic_write("  #%d  0x%016lx  %s+0x%lx (%s:%u)",
            index, addr, sym.name, sym.offset, loc.file, loc.line);
    } else if (has_sym) {
        log::panic_write("  #%d  0x%016lx  %s+0x%lx", index, addr, sym.name, sym.offset);
    } else if (has_line) {
        log::panic_write("  #%d  0x%016lx  (%s:%u)", index, addr, loc.file, loc.line);
    } else {
        log::panic_write("  #%d  0x%016lx", index, addr);
    }
}

static void print_stacktrace(uint64_t rbp, uint64_t rip) {
    log::panic_write("");
    log::panic_write("Stack trace:");

    print_frame(0, rip);

    stacktrace::frame frames[stacktrace::MAX_FRAMES];
    int depth = stacktrace::walk(rbp, frames, stacktrace::MAX_FRAMES);

    for (int i = 0; i < depth; i++) {
        print_frame(i + 1, frames[i].return_addr);
    }
}

static void print_registers(const x86::trap_frame* tf, uint64_t cr2) {
    log::panic_write("");
    log::panic_write("Registers:");
    log::panic_write("  rax=0x%016lx  rbx=0x%016lx  rcx=0x%016lx", tf->rax, tf->rbx, tf->rcx);
    log::panic_write("  rdx=0x%016lx  rsi=0x%016lx  rdi=0x%016lx", tf->rdx, tf->rsi, tf->rdi);
    log::panic_write("  rbp=0x%016lx  rsp=0x%016lx  rip=0x%016lx", tf->rbp, tf->rsp, tf->rip);
    log::panic_write("  r8 =0x%016lx  r9 =0x%016lx  r10=0x%016lx", tf->r8, tf->r9, tf->r10);
    log::panic_write("  r11=0x%016lx  r12=0x%016lx  r13=0x%016lx", tf->r11, tf->r12, tf->r13);
    log::panic_write("  r14=0x%016lx  r15=0x%016lx", tf->r14, tf->r15);
    log::panic_write("   cs=0x%016lx   ss=0x%016lx  rflags=0x%lx", tf->cs, tf->ss, tf->rflags);
    log::panic_write("  cr2=0x%016lx", cr2);
}

[[noreturn]] __PRIVILEGED_CODE void on_trap(x86::trap_frame* tf) {
    cpu::irq_disable();

    uint64_t cr2 = 0;
    if (tf->vector == x86::EXC_PAGE_FAULT) cr2 = x86::read_cr2();

    log::panic_write("");
    log::panic_write("================================================================================");
    log::panic_write("KERNEL PANIC: %s", exception_name(tf->vector));
    log::panic_write("================================================================================");
    log::panic_write("");

    if (tf->vector == x86::EXC_PAGE_FAULT) {
        print_pf_details(tf->error_code, cr2);
    } else if (tf->error_code != 0) {
        log::panic_write("  Error code: 0x%04lx", tf->error_code);
    }

    print_stacktrace(tf->rbp, tf->rip);
    print_registers(tf, cr2);

    log::panic_write("");
    const char* mode = x86::from_user(tf) ? "user" : "supervisor";
    sched::task* cur = sched::current();
    if (cur && cur->name) {
        log::panic_write("  Mode: %s | Task: %s (tid=%u) | CPU: %u", mode, cur->name, cur->tid, this_cpu(percpu_cpu_id));
    } else {
        log::panic_write("  Mode: %s", mode);
    }
    log::panic_write("================================================================================");

    for (;;) cpu::halt();
}

} // namespace panic
