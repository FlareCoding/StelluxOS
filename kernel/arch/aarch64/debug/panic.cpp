#include "debug/panic.h"
#include "defs/exception.h"
#include "trap/trap_frame.h"
#include "debug/symtab.h"
#include "debug/dwarf_line.h"
#include "debug/stacktrace.h"
#include "common/logging.h"
#include "hw/cpu.h"
#include "mm/paging.h"
#include "mm/paging_arch.h"
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

static void print_ttbr0_walk(uint64_t ttbr0_root, uint64_t va) {
    if (ttbr0_root == 0) {
        log::panic_write("TTBR0 walk: root is 0");
        return;
    }

    constexpr uint64_t DESC_ADDR_MASK_4K = 0x0000FFFFFFFFF000ULL;
    auto parts = paging::split_virt_addr(va);
    auto* l0 = static_cast<paging::translation_table_t*>(paging::phys_to_virt(ttbr0_root));
    uint64_t l0e = l0->raw[parts.l0_idx];
    log::panic_write("TTBR0 walk: L0[%u]=0x%016lx", static_cast<uint32_t>(parts.l0_idx), l0e);
    if ((l0e & 1ULL) == 0 || (l0e & 2ULL) == 0) {
        return;
    }

    uint64_t l1_phys = l0e & DESC_ADDR_MASK_4K;
    auto* l1 = static_cast<paging::translation_table_t*>(paging::phys_to_virt(l1_phys));
    uint64_t l1e = l1->raw[parts.l1_idx];
    log::panic_write("           L1[%u]=0x%016lx", static_cast<uint32_t>(parts.l1_idx), l1e);
    if ((l1e & 1ULL) == 0) {
        return;
    }
    if ((l1e & 2ULL) == 0) {
        log::panic_write("           -> 1GB block at L1");
        return;
    }

    uint64_t l2_phys = l1e & DESC_ADDR_MASK_4K;
    auto* l2 = static_cast<paging::translation_table_t*>(paging::phys_to_virt(l2_phys));
    uint64_t l2e = l2->raw[parts.l2_idx];
    log::panic_write("           L2[%u]=0x%016lx", static_cast<uint32_t>(parts.l2_idx), l2e);
    if ((l2e & 1ULL) == 0) {
        return;
    }
    if ((l2e & 2ULL) == 0) {
        log::panic_write("           -> 2MB block at L2");
        return;
    }

    uint64_t l3_phys = l2e & DESC_ADDR_MASK_4K;
    auto* l3 = static_cast<paging::translation_table_t*>(paging::phys_to_virt(l3_phys));
    uint64_t l3e = l3->raw[parts.l3_idx];
    log::panic_write("           L3[%u]=0x%016lx", static_cast<uint32_t>(parts.l3_idx), l3e);
}

[[noreturn]] __PRIVILEGED_CODE void on_trap(aarch64::trap_frame* tf, const char* kind) {
    cpu::irq_disable();

    uint64_t esr = tf->esr;
    uint8_t ec = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);
    uint64_t tcr = paging::read_tcr_el1();
    uint64_t ttbr0 = paging::read_ttbr0_el1();
    uint64_t ttbr1 = paging::read_ttbr1_el1();
    uint64_t t0sz = tcr & 0x3FULL;
    uint64_t t1sz = (tcr >> 16) & 0x3FULL;
    uint64_t va_bits_ttbr0 = 64 - t0sz;
    uint64_t va_bits_ttbr1 = 64 - t1sz;

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
    log::panic_write(
        "MMU state: TCR_EL1=0x%016lx (T0SZ=%lu VA_BITS0=%lu | T1SZ=%lu VA_BITS1=%lu)",
        tcr, t0sz, va_bits_ttbr0, t1sz, va_bits_ttbr1
    );
    log::panic_write("           TTBR0_EL1=0x%016lx TTBR1_EL1=0x%016lx", ttbr0, ttbr1);
    print_ttbr0_walk(ttbr0, tf->far);
    paging::at_s1e0r(tf->far);
    uint64_t par_far = paging::read_par_el1();
    bool par_far_fault = (par_far & 1ULL) != 0;
    log::panic_write(
        "           AT S1E0R(FAR)=0x%016lx -> PAR_EL1=0x%016lx (%s)",
        tf->far, par_far, par_far_fault ? "fault" : "ok"
    );

    log::panic_write("");
    const char* mode = aarch64::from_user(tf) ? "user (EL0)" : "kernel (EL1)";
    sched::task* cur = sched::current();
    uint32_t cpu_id = this_cpu(percpu_cpu_id);
    if (cur && cur->name) {
        log::panic_write("  Mode: %s | Task: %s (tid=%u) | CPU: %u", mode, cur->name, cur->tid, cpu_id);
    } else {
        log::panic_write("  Mode: %s | CPU: %u | Task: (null)", mode, cpu_id);
    }
    log::panic_write("================================================================================");

    for (;;) cpu::halt();
}

} // namespace panic
