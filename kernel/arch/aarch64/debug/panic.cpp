#include "debug/panic.h"
#include "defs/exception.h"
#include "trap/trap_frame.h"
#include "debug/symtab.h"
#include "debug/dwarf_line.h"
#include "debug/stacktrace.h"
#include "common/logging.h"
#include "hw/cpu.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sched/task.h"

namespace panic {

static const char* ec_name(uint8_t ec) {
    switch (ec) {
        case 0x00: return "Unknown Reason";
        case 0x01: return "Trapped WFI/WFE";
        case 0x0E: return "Illegal Execution State";
        case 0x15: return "SVC (AArch64)";
        case 0x20: return "Instruction Abort (lower EL)";
        case 0x21: return "Instruction Abort (same EL)";
        case 0x22: return "PC Alignment Fault";
        case 0x24: return "Data Abort (lower EL)";
        case 0x25: return "Data Abort (same EL)";
        case 0x26: return "SP Alignment Fault";
        case 0x2C: return "Trapped FP (AArch64)";
        case 0x2F: return "SError";
        case 0x30: return "Breakpoint (lower EL)";
        case 0x31: return "Breakpoint (same EL)";
        case 0x32: return "Software Step (lower EL)";
        case 0x33: return "Software Step (same EL)";
        case 0x34: return "Watchpoint (lower EL)";
        case 0x35: return "Watchpoint (same EL)";
        case 0x3C: return "BRK (AArch64)";
        default:   return "Reserved";
    }
}

static const char* dfsc_name(uint8_t dfsc) {
    switch (dfsc & 0x3F) {
        case 0x00: return "Address size fault, level 0";
        case 0x01: return "Address size fault, level 1";
        case 0x02: return "Address size fault, level 2";
        case 0x03: return "Address size fault, level 3";
        case 0x04: return "Translation fault, level 0";
        case 0x05: return "Translation fault, level 1";
        case 0x06: return "Translation fault, level 2";
        case 0x07: return "Translation fault, level 3";
        case 0x09: return "Access flag fault, level 1";
        case 0x0A: return "Access flag fault, level 2";
        case 0x0B: return "Access flag fault, level 3";
        case 0x0D: return "Permission fault, level 1";
        case 0x0E: return "Permission fault, level 2";
        case 0x0F: return "Permission fault, level 3";
        case 0x10: return "Synchronous external abort";
        case 0x21: return "Alignment fault";
        default:   return "Unknown fault";
    }
}

static bool is_data_abort(uint8_t ec) {
    return ec == aarch64::EC_DATA_ABORT_LOWER || ec == aarch64::EC_DATA_ABORT_SAME;
}

static bool is_instruction_abort(uint8_t ec) {
    return ec == aarch64::EC_INST_ABORT_LOWER || ec == aarch64::EC_INST_ABORT_SAME;
}

static void print_abort_details(const aarch64::trap_frame* tf) {
    uint64_t esr = tf->esr;
    uint8_t ec  = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);
    uint32_t iss = static_cast<uint32_t>(esr & aarch64::ESR_ISS_MASK);
    uint8_t dfsc = static_cast<uint8_t>(iss & 0x3F);

    log::panic_write("  Faulting address (FAR): 0x%016lx", tf->far);
    log::panic_write("  ESR: 0x%08lx (EC=0x%02x: %s | %cFSC=0x%02x: %s)",
        esr, ec, ec_name(ec),
        is_data_abort(ec) ? 'D' : 'I',
        dfsc, dfsc_name(dfsc));

    if (is_data_abort(ec)) {
        const char* rw = (iss & (1 << 6)) ? "write" : "read";
        if (tf->far < 0x1000) {
            if (tf->far == 0) {
                log::panic_write("  -> Null pointer dereference");
            } else {
                log::panic_write("  -> Null pointer dereference (%s at offset 0x%lx from null)", rw, tf->far);
            }
        }
    } else if (is_instruction_abort(ec)) {
        if (tf->far < 0x1000) {
            log::panic_write("  -> Jump/call through null function pointer");
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

static void print_stacktrace(const aarch64::trap_frame* tf) {
    log::panic_write("");
    log::panic_write("Stack trace:");

    print_frame(0, tf->elr);

    uint64_t fp = tf->x[29];
    stacktrace::frame frames[stacktrace::MAX_FRAMES];
    int depth = stacktrace::walk(fp, frames, stacktrace::MAX_FRAMES);

    for (int i = 0; i < depth; i++) {
        print_frame(i + 1, frames[i].return_addr);
    }
}

static void print_registers(const aarch64::trap_frame* tf) {
    log::panic_write("");
    log::panic_write("Registers:");
    for (int i = 0; i < 30; i += 3) {
        if (i + 2 < 30) {
            log::panic_write("  x%02d=0x%016lx  x%02d=0x%016lx  x%02d=0x%016lx",
                i, tf->x[i], i+1, tf->x[i+1], i+2, tf->x[i+2]);
        } else if (i + 1 < 30) {
            log::panic_write("  x%02d=0x%016lx  x%02d=0x%016lx",
                i, tf->x[i], i+1, tf->x[i+1]);
        } else {
            log::panic_write("  x%02d=0x%016lx", i, tf->x[i]);
        }
    }
    log::panic_write("  x30=0x%016lx  (LR)", tf->x[30]);
    log::panic_write("   sp=0x%016lx  elr=0x%016lx  spsr=0x%08lx", tf->sp, tf->elr, tf->spsr);
}

[[noreturn]] __PRIVILEGED_CODE void on_trap(aarch64::trap_frame* tf, const char* kind) {
    if (!dynpriv::is_elevated()) {
        dynpriv::elevate();
    }
    cpu::irq_disable();

    uint64_t esr = tf->esr;
    uint8_t ec = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);

    log::panic_write("");
    log::panic_write("================================================================================");
    log::panic_write("KERNEL PANIC: %s (%s)", ec_name(ec), kind);
    log::panic_write("================================================================================");
    log::panic_write("");

    if (is_data_abort(ec) || is_instruction_abort(ec)) {
        print_abort_details(tf);
    } else {
        log::panic_write("  ESR: 0x%08lx (EC=0x%02x: %s)", esr, ec, ec_name(ec));
    }

    print_stacktrace(tf);
    print_registers(tf);

    log::panic_write("");
    const char* mode = aarch64::from_user(tf) ? "user (EL0)" : "kernel (EL1)";
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
