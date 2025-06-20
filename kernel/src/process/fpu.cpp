#include <process/fpu.h>
#include <process/process_core.h>
#include <process/process.h>
#include <arch/x86/cpu_control.h>
#include <memory/memory.h>
#include <core/klog.h>

// FPU management per-CPU variables
DEFINE_PER_CPU(process_core*, fpu_owner);
DEFINE_PER_CPU(bool, fpu_used_in_irq);

namespace fpu {

__PRIVILEGED_CODE
void init_fpu_state(process_core* core) {
    if (!core) {
        return;
    }

    // Zero out the FPU state structure
    zeromem(&core->fpu_context, sizeof(fpu_state));
    
    // Initialize the FXSAVE area with default FPU state
    // FXSAVE area format (simplified):
    // Bytes 0-1: FCW (FPU Control Word) - default 0x037F
    // Bytes 2-3: FSW (FPU Status Word) - default 0x0000
    // Bytes 4-5: FTW (FPU Tag Word) - default 0xFFFF
    // Bytes 6-7: FOP (FPU Opcode) - default 0x0000
    // Bytes 24-27: MXCSR (SSE Control/Status) - default 0x1F80
    // Bytes 28-31: MXCSR_MASK - default 0xFFFF
    
    uint16_t* fcw = reinterpret_cast<uint16_t*>(&core->fpu_context.fxsave_area[0]);
    uint16_t* fsw = reinterpret_cast<uint16_t*>(&core->fpu_context.fxsave_area[2]);
    uint16_t* ftw = reinterpret_cast<uint16_t*>(&core->fpu_context.fxsave_area[4]);
    uint32_t* mxcsr = reinterpret_cast<uint32_t*>(&core->fpu_context.fxsave_area[24]);
    uint32_t* mxcsr_mask = reinterpret_cast<uint32_t*>(&core->fpu_context.fxsave_area[28]);
    
    *fcw = 0x037F;          // Default FPU control word
    *fsw = 0x0000;          // Clear status word
    *ftw = 0xFFFF;          // All tags empty
    *mxcsr = 0x1F80;        // Default MXCSR (mask all exceptions)
    *mxcsr_mask = 0xFFFF;   // Default MXCSR mask
    
    // Set FPU state as initialized but not used
    core->fpu_context.fpu_initialized = 1;
    core->fpu_context.has_used_fpu = 0;
    core->fpu_context.needs_fpu_save = 0;
}

__PRIVILEGED_CODE
void save_fpu_state(process_core* core) {
    if (!core || !core->fpu_context.fpu_initialized) {
        return;
    }
    
    // Safety check: Ensure FPU is enabled before FXSAVE
    if (!is_enabled()) {
        kprint("[FPU] Warning: Attempting to save FPU state while FPU is disabled!\n");
        enable();  // Enable FPU before saving
    }
    
    // Verify alignment (debug check)
    uintptr_t addr = reinterpret_cast<uintptr_t>(&core->fpu_context.fxsave_area[0]);
    if ((addr & 0xF) != 0) {
        kprint("[FPU] FATAL: FXSAVE area not 16-byte aligned!\n");
        kprint("[FPU]   &current = 0x%llx\n", current);
        kprint("[FPU]   fxsave_area address: 0x%016llx\n", (uint64_t)addr);
        kprint("[FPU]   alignment offset: %llu bytes\n", (uint64_t)(addr & 0xF));
        kprint("[FPU]   fpu_context address: 0x%016llx\n", (uint64_t)&core->fpu_context);
        kprint("[FPU]   process_core address: 0x%016llx\n", (uint64_t)core);
        panic("FXSAVE area alignment violation");
    }
    
    // Save FPU state using FXSAVE instruction
    asm volatile("fxsave %0" : "=m" (core->fpu_context.fxsave_area));
    
    // Mark that FPU state has been saved
    core->fpu_context.needs_fpu_save = 0;
}

__PRIVILEGED_CODE
void restore_fpu_state(process_core* core) {
    if (!core || !core->fpu_context.fpu_initialized) {
        return;
    }
    
    // Restore FPU state using FXRSTOR instruction
    asm volatile("fxrstor %0" : : "m" (core->fpu_context.fxsave_area));
}

__PRIVILEGED_CODE
void clear_fpu_state() {
    // Clear FPU state for security - zero all FPU/SSE registers
    asm volatile("finit");                  // Initialize FPU (clears x87 state)
    asm volatile("pxor %xmm0, %xmm0");      // Clear XMM registers
    asm volatile("pxor %xmm1, %xmm1");
    asm volatile("pxor %xmm2, %xmm2");
    asm volatile("pxor %xmm3, %xmm3");
    asm volatile("pxor %xmm4, %xmm4");
    asm volatile("pxor %xmm5, %xmm5");
    asm volatile("pxor %xmm6, %xmm6");
    asm volatile("pxor %xmm7, %xmm7");
    asm volatile("pxor %xmm8, %xmm8");
    asm volatile("pxor %xmm9, %xmm9");
    asm volatile("pxor %xmm10, %xmm10");
    asm volatile("pxor %xmm11, %xmm11");
    asm volatile("pxor %xmm12, %xmm12");
    asm volatile("pxor %xmm13, %xmm13");
    asm volatile("pxor %xmm14, %xmm14");
    asm volatile("pxor %xmm15, %xmm15");
}

__PRIVILEGED_CODE
void enable() {
    arch::x86::fpu_enable();
}

__PRIVILEGED_CODE
void disable() {
    arch::x86::fpu_disable();
}

__PRIVILEGED_CODE
bool is_enabled() {
    return arch::x86::is_fpu_enabled();
}

#ifdef ARCH_X86_64

DEFINE_INT_HANDLER(exc_nm_fpu_handler) {
    __unused regs;
    __unused cookie;
    
    // Check if we have a valid current process
    process* current_process = current;
    if (!current_process) {
        kprint("[FPU] #NM: No current process! Panic.\n");
        panic("FPU #NM exception with no current process");
        return IRQ_HANDLED;
    }
    
    process_core* current_core = current_process->get_core();
    if (!current_core) {
        kprint("[FPU] #NM: No current process core! Panic.\n");
        panic("FPU #NM exception with no current process core");
        return IRQ_HANDLED;
    }
    
    // Check if FPU state is initialized
    if (!current_core->fpu_context.fpu_initialized) {
        kprint("[FPU] #NM: Process FPU state not initialized! Initializing...\n");
        init_fpu_state(current_core);
    }
    
    // Check if we're in interrupt context (shouldn't normally happen)
    bool in_irq_context = this_cpu_read(fpu_used_in_irq);
    if (in_irq_context) {
        kprint("[FPU] #NM: Warning - FPU access in IRQ context!\n");
        // Allow it but log warning - some kernel code might need FPU
    }
    
    // Get current FPU owner for this CPU
    process_core* current_fpu_owner = this_cpu_read(fpu_owner);
    
    // Display ownership information
#if 0
    if (current_fpu_owner && current_fpu_owner != current_core) {
        kprint("[FPU] #NM: PID %lli ('%s') FPU use, previous owner: PID %lli ('%s')\n",
               current_core->identity.pid, 
               current_core->identity.name,
               current_fpu_owner->identity.pid,
               current_fpu_owner->identity.name);
    } else if (current_fpu_owner == current_core) {
        kprint("[FPU] #NM: PID %lli ('%s') FPU use, already owns FPU\n",
               current_core->identity.pid, 
               current_core->identity.name);
    } else {
        kprint("[FPU] #NM: PID %lli ('%s') FPU use, previous owner: none\n",
               current_core->identity.pid, 
               current_core->identity.name);
    }
#endif

    // Save current FPU owner's state if needed
    if (current_fpu_owner && current_fpu_owner != current_core) {
        if (current_fpu_owner->fpu_context.needs_fpu_save) {
#if 0
            kprint("[FPU] #NM: Saving previous owner's FPU state\n");
#endif
            save_fpu_state(current_fpu_owner);
        }
    }
    
    // Enable FPU (clear TS bit in CR0)
    enable();
    
    // Verify FPU is now enabled
    if (!is_enabled()) {
        kprint("[FPU] #NM: Failed to enable FPU! Panic.\n");
        panic("Failed to enable FPU after CLTS");
        return IRQ_HANDLED;
    }
    
    // Load FPU state for current process
    if (current_core->fpu_context.has_used_fpu) {
        // Process has used FPU before - restore its state
#if 0
        kprint("[FPU] #NM: Restoring process FPU state\n");
#endif

        // Safety check: verify the FXSAVE area is properly aligned before restore
        uintptr_t addr = reinterpret_cast<uintptr_t>(&current_core->fpu_context.fxsave_area[0]);
        if ((addr & 0xF) != 0) {
            kprint("[FPU] FATAL: FXRSTOR area not 16-byte aligned: 0x%016llx\n", (uint64_t)addr);
            panic("FXRSTOR area alignment violation");
        }
        
        restore_fpu_state(current_core);
    } else {
        // First time FPU use for this process - initialize clean state
#if 0
        kprint("[FPU] #NM: First FPU use - initializing clean state\n");
#endif
        clear_fpu_state();
        current_core->fpu_context.has_used_fpu = 1;
    }
    
    // Update FPU ownership tracking
    this_cpu_write(fpu_owner, current_core);
    current_core->fpu_context.needs_fpu_save = 1;

#if 0    
    kprint("[FPU] #NM: Lazy FPU loading complete, resuming execution\n");
#endif

    return IRQ_HANDLED; // Return to retry the faulting FPU instruction
}

#endif // ARCH_X86_64
} // namespace fpu
 