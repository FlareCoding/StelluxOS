.intel_syntax noprefix
.code64

#
# Sets the current CPU state to the given context and uses iret to switch context 
# C function signature:
#      void __asm_switch_cpu_context_and_iret(CpuContext* ctx)
#
.global __asm_switch_cpu_context_and_iret

# PtRegs Struct:
# struct CpuContext {
#     // General purpose registers
#     uint64_t rax, rbx, rcx, rdx;
#     uint64_t rsi, rdi, rbp;
#     uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

#     // Stack pointer, instruction pointer, and flags register
#     uint64_t rip, rsp, rflags;

#     // Segment registers
#     uint64_t cs, ds, es, fs, gs, ss;
# } __attribute__((packed));

.equ cpu_context_rax_offset, 0x00
.equ cpu_context_rbx_offset, 0x08
.equ cpu_context_rcx_offset, 0x10
.equ cpu_context_rdx_offset, 0x18
.equ cpu_context_rsi_offset, 0x20
.equ cpu_context_rdi_offset, 0x28
.equ cpu_context_rbp_offset, 0x30
.equ cpu_context_r8_offset,  0x38
.equ cpu_context_r9_offset,  0x40
.equ cpu_context_r10_offset, 0x48
.equ cpu_context_r11_offset, 0x50
.equ cpu_context_r12_offset, 0x58
.equ cpu_context_r13_offset, 0x60
.equ cpu_context_r14_offset, 0x68
.equ cpu_context_r15_offset, 0x70
.equ cpu_context_rip_offset, 0x78
.equ cpu_context_rsp_offset, 0x80
.equ cpu_context_rflags_offset, 0x88
.equ cpu_context_cs_offset,  0x90
.equ cpu_context_ds_offset,  0x98
.equ cpu_context_es_offset,  0xA0
.equ cpu_context_fs_offset,  0xA8
.equ cpu_context_gs_offset,  0xB0
.equ cpu_context_ss_offset,  0xB8

#
# rdi --> CpuContext* ctx
#
__asm_switch_cpu_context_and_iret:
    # Preserve rdi and rbp
    mov rax, [rdi + cpu_context_rdi_offset]
    push rax

    mov rax, [rdi + cpu_context_rbp_offset]
    push rax

    # Load general purpose registers
    xor rax, rax
    mov rax, [rdi + cpu_context_ds_offset]
    mov ds, ax

    mov rax, [rdi + cpu_context_gs_offset]
    mov gs, ax

    mov rax, [rdi + cpu_context_fs_offset]
    mov fs, ax

    mov rax, [rdi + cpu_context_es_offset]
    mov es, ax

    mov rax, [rdi + cpu_context_rax_offset]
    mov rbx, [rdi + cpu_context_rbx_offset]
    mov rcx, [rdi + cpu_context_rcx_offset]
    mov rdx, [rdi + cpu_context_rdx_offset]

    mov rsi, [rdi + cpu_context_rsi_offset]

    mov r8,  [rdi + cpu_context_r8_offset]
    mov r9,  [rdi + cpu_context_r9_offset]
    mov r10, [rdi + cpu_context_r10_offset]
    mov r11, [rdi + cpu_context_r11_offset]
    mov r12, [rdi + cpu_context_r12_offset]
    mov r13, [rdi + cpu_context_r13_offset]
    mov r14, [rdi + cpu_context_r14_offset]
    mov r15, [rdi + cpu_context_r15_offset]

    # Restore the correct rbp value from the stack
    pop rbp

    # Construct an interrupt frame on stack
	push	[rdi + cpu_context_ss_offset]       # regs.hwframe->ss
	push    [rdi + cpu_context_rsp_offset]      # regs.hwframe->rsp
	push	[rdi + cpu_context_rflags_offset]   # regs.hwframe->rflags
	push	[rdi + cpu_context_cs_offset]	    # regs.hwframe->cs
	push	[rdi + cpu_context_rip_offset]	    # regs.hwframe->rip

    # Restore the new context's rdi
    mov rdi, [rsp + 0x28]

    # Execute iretq to switch to the new context
    iretq

.section .note.GNU-stack,"",@progbits
