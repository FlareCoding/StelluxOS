.intel_syntax noprefix
.code64

.equ tss_offset_rsp0,  0x04
.equ tss_offset_rsp1,  0x0C
.equ tss_offset_rsp2,  0x14

.equ __KERNEL_CS,   0x08
.equ __USER_CS,     0x33
.equ __USER_DS,     0x2b

.extern __syscall_handler
.global __asm_syscall_entry64

.text
__asm_syscall_entry64:
    # Switch to kernel gs
    swapgs

    # Switch to kernel stack
    mov gs:[tss_offset_rsp2], rsp # rsp2 is at offset 0x14 in the TSS (store the user stack)
    mov rsp, gs:[tss_offset_rsp0]  # rsp0 is at offset 0x04 in the TSS (retrieve the kernel stack)

    # Comment this out to take the iret path
    jmp _ignored_iret_construction_path

    # Construct an interrupt frame on stack
	push	__USER_DS				# regs.hwframe->ss
	push    gs:[tss_offset_rsp2]    # regs.hwframe->rsp
	push	r11					    # regs.hwframe->rflags
	push	__USER_CS				# regs.hwframe->cs
	push	rcx					    # regs.hwframe->rip

_ignored_iret_construction_path:
    # Save volatile registers that we are going to modify
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    push r10
    push r11

    # Move syscall arguments into appropriate registers
    mov r9,  r8   # arg5
    mov r8,  r10  # arg4
    mov r10, rdx  # arg3
    mov rdx, rsi  # arg2
    mov rsi, rdi  # arg1
    mov rdi, rax  # syscall number

    # Call the C function
    call __syscall_handler

    # Restore volatile registers
    pop r11
    pop r10
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi

    # Uncomment this to take the iret path
    # jmp __syscall_exit_swapgs_and_iret

__syscall_exit_swapgs_and_sysret:
    mov rsp, gs:[tss_offset_rsp2]
    swapgs

    sysretq

__syscall_exit_swapgs_and_iret:
    # Switch back to user gs
    swapgs

    iretq

.section .note.GNU-stack, "", @progbits
