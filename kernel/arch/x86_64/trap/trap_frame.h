#ifndef STELLUX_ARCH_X86_64_TRAP_TRAP_FRAME_H
#define STELLUX_ARCH_X86_64_TRAP_TRAP_FRAME_H

#include "types.h"

namespace x86 {

struct trap_frame {
    uint64_t r15; uint64_t r14; uint64_t r13; uint64_t r12;
    uint64_t r11; uint64_t r10; uint64_t r9; uint64_t r8;
    uint64_t rbp; uint64_t rdi; uint64_t rsi; uint64_t rdx;
    uint64_t rcx; uint64_t rbx; uint64_t rax;

    uint64_t vector;
    uint64_t error_code;

    // hardware iret frame
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

// Field offset checks matching entry.S assembly offsets
static_assert(__builtin_offsetof(trap_frame, r15) == 0x00);
static_assert(__builtin_offsetof(trap_frame, r14) == 0x08);
static_assert(__builtin_offsetof(trap_frame, r13) == 0x10);
static_assert(__builtin_offsetof(trap_frame, r12) == 0x18);
static_assert(__builtin_offsetof(trap_frame, r11) == 0x20);
static_assert(__builtin_offsetof(trap_frame, r10) == 0x28);
static_assert(__builtin_offsetof(trap_frame, r9) == 0x30);
static_assert(__builtin_offsetof(trap_frame, r8) == 0x38);
static_assert(__builtin_offsetof(trap_frame, rbp) == 0x40);
static_assert(__builtin_offsetof(trap_frame, rdi) == 0x48);
static_assert(__builtin_offsetof(trap_frame, rsi) == 0x50);
static_assert(__builtin_offsetof(trap_frame, rdx) == 0x58);
static_assert(__builtin_offsetof(trap_frame, rcx) == 0x60);
static_assert(__builtin_offsetof(trap_frame, rbx) == 0x68);
static_assert(__builtin_offsetof(trap_frame, rax) == 0x70);
static_assert(__builtin_offsetof(trap_frame, vector) == 0x78);
static_assert(__builtin_offsetof(trap_frame, error_code) == 0x80);
static_assert(__builtin_offsetof(trap_frame, rip) == 0x88);
static_assert(__builtin_offsetof(trap_frame, cs) == 0x90);
static_assert(__builtin_offsetof(trap_frame, rflags) == 0x98);
static_assert(__builtin_offsetof(trap_frame, rsp) == 0xA0);
static_assert(__builtin_offsetof(trap_frame, ss) == 0xA8);

// Size check matching entry.S TF_SIZE
static_assert(sizeof(trap_frame) == 0xB0);

// Alignment check: structure should be naturally aligned (8-byte aligned)
static_assert(alignof(trap_frame) == 0x08);

inline uint64_t get_ip(const trap_frame* tf) {
    return tf->rip;
}

inline uint64_t get_sp(const trap_frame* tf) {
    return tf->rsp;
}

inline bool from_user(const trap_frame* tf) {
    return (tf->cs & 3) == 3;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline uint64_t read_cr2() {
    uint64_t v;
    asm volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

} // namespace x86

#endif // STELLUX_ARCH_X86_64_TRAP_TRAP_FRAME_H

