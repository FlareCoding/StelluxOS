# Stellux 3.0 Scheduler Design

> **Status:** Deep design discussion — tracing through every step.

---

## Part 1: The Context Switch — Step by Step

### The Core Idea

A context switch is: save the current task's full CPU state, load another task's full CPU state, and resume it.

The existing trap entry/exit path **already does this** for every interrupt:
- Entry: saves every register into a `trap_frame` on the system stack
- Exit: restores every register from that `trap_frame` and `iretq`/`eret`

If `yield()` triggers a dedicated interrupt, the only thing the scheduler needs to do is **swap which trap_frame the exit path restores from.** That's it. The entry saves the yielding task's state. The handler overwrites the trap_frame with the next task's saved state. The exit restores the new state. Done.

No callee-saved register saves. No stack switching. No trampolines. No synthetic frames. The trap infrastructure already handles every detail — privilege transitions, swapgs, iretq/eret.

### Step-by-Step Trace: x86_64

Let's say Task A (elevated, at Ring 0) wants to yield. Task B was previously saved.

#### Step 1: Task A triggers the yield interrupt

```
Task A executes:  int SCHED_VECTOR
```

The CPU (hardware) does:
1. Pushes SS, RSP, RFLAGS, CS, RIP onto the current stack (since Ring 0 → Ring 0, no stack switch via TSS)
2. Loads RIP from IDT[SCHED_VECTOR] — jumps to `stlx_x86_isr_SCHED_VECTOR`

#### Step 2: The ISR stub normalizes the stack

```asm
stlx_x86_isr_SCHED_VECTOR:
    push 0                    /* error_code = 0 (this vector has no hardware error code) */
    push SCHED_VECTOR         /* vector number */
    jmp stlx_x86_common_entry
```

Stack now (growing downward):
```
    vector          = SCHED_VECTOR
    error_code      = 0
    RIP             = Task A's return address (instruction after `int`)
    CS              = Task A's code segment (kernel CS, Ring 0)
    RFLAGS          = Task A's flags
    RSP             = Task A's stack pointer (before `int`)
    SS              = Task A's stack segment
```

#### Step 3: Common entry saves all GPRs

```asm
stlx_x86_common_entry:
    cld
    sub rsp, TF_SIZE              /* allocate trap_frame space */
    mov [rsp + TF_R15], r15       /* save all 15 GPRs */
    mov [rsp + TF_R14], r14
    ... (all GPRs saved) ...
    mov [rsp + TF_RAX], rax

    /* Copy the header (vector, error, iret frame) into the trap_frame */
    lea rsi, [rsp + TF_SIZE]
    mov rax, [rsi + HDR_VECTOR]
    mov [rsp + TF_VECTOR], rax
    ... (all 7 fields copied) ...
```

Now the trap_frame at `[rsp]` contains Task A's **complete** CPU state:
```
trap_frame (at rsp):
    R15, R14, R13, R12, R11, R10, R9, R8     — Task A's GPRs
    RBP, RDI, RSI, RDX, RCX, RBX, RAX        — Task A's GPRs
    vector = SCHED_VECTOR                      — identifies why we're here
    error_code = 0
    RIP = Task A's next instruction            — where A resumes
    CS = Task A's code segment                 — Ring 0
    RFLAGS = Task A's flags                    — interrupt flag state, etc.
    RSP = Task A's stack pointer               — A's stack before the int
    SS = Task A's stack segment
```

This is **everything** needed to resume Task A later.

#### Step 4: Entry handles privilege (swapgs, stack switch for elevated tasks)

The entry code checks CS to determine privilege origin and handles swapgs / elevated stack switch as needed. For an elevated kernel task, the frame may be copied to the system stack. The specifics depend on the task's flags, but the result is the same: `rsp` points to a valid trap_frame on the system stack.

#### Step 5: Call the C handler

```asm
.Lstlx_x86_call_c:
    mov rdi, rsp          /* arg0 = pointer to trap_frame */
    mov r12, rsp          /* backup in callee-saved r12 */
    and rsp, -16          /* align stack for ABI */
    call stlx_x86_64_trap_handler
    mov rsp, r12          /* restore rsp = pointer to trap_frame */
```

The C handler receives `trap_frame* tf`, which points directly to the trap_frame on the stack. **The handler can read and write this trap_frame freely.** Whatever values are in the trap_frame when the handler returns is what gets restored.

#### Step 6: The trap handler dispatches to the scheduler

```cpp
extern "C" void stlx_x86_64_trap_handler(x86::trap_frame* tf) {
    sched::task_exec_core* task = this_cpu(current_task);
    task->flags |= sched::TASK_FLAG_IN_IRQ;

    if (tf->vector == x86::SCHED_VECTOR) {
        sched::schedule(tf);    // <-- THIS IS THE ENTIRE CONTEXT SWITCH
        task = this_cpu(current_task);  // current_task may have changed!
    }
    // ... other vectors (page fault, etc.) ...

    task->flags &= ~sched::TASK_FLAG_IN_IRQ;
}
```

#### Step 7: `schedule(tf)` — the actual context switch

```cpp
void sched::schedule(trap_frame* tf) {
    task* prev = current();
    task* next = /* pick next task via policy */;

    if (prev == next) return;  // nothing to do

    // 1. Save Task A's complete state
    prev->saved_frame = *tf;       // copy trap_frame to Task A's task struct

    // 2. Load Task B's complete state
    *tf = next->saved_frame;       // overwrite trap_frame with Task B's state

    // 3. Update bookkeeping
    prev->state = task_state::ready;
    next->state = task_state::running;
    this_cpu(current_task) = &next->exec;

    // That's it. When this function returns, the trap_frame on the stack
    // now contains Task B's register values, including B's RIP, RSP, CS, etc.
}
```

This is the entire context switch. Two struct copies. No assembly. No stack switching. No callee-saved saves. No trampolines.

#### Step 8: Handler returns, exit code runs

The handler returns. The assembly does:

```asm
    mov rsp, r12          /* rsp = pointer to trap_frame (same address, but contents changed!) */
```

`r12` still points to the same memory location on the stack. But that memory now contains Task B's register values (because `schedule()` overwrote it in step 7).

#### Step 9: Write back trap_frame to the iret frame

```asm
    lea rsi, [rsp + TF_SIZE]       /* rsi = header above trap_frame */
    mov rax, [rsp + TF_RIP]        /* Task B's RIP */
    mov [rsi + HDR_RIP], rax       /* write to iret frame */
    mov rax, [rsp + TF_CS]         /* Task B's CS */
    mov [rsi + HDR_CS], rax
    mov rax, [rsp + TF_RFLAGS]     /* Task B's RFLAGS */
    mov [rsi + HDR_RFLAGS], rax
    mov rax, [rsp + TF_RSP]        /* Task B's RSP */
    mov [rsi + HDR_RSP], rax
    mov rax, [rsp + TF_SS]         /* Task B's SS */
    mov [rsi + HDR_SS], rax
```

Now the iret frame on the stack has Task B's instruction pointer, stack, privilege level, and flags.

#### Step 10: Restore GPRs

```asm
    mov r15, [rsp + TF_R15]        /* Task B's R15 */
    mov r14, [rsp + TF_R14]        /* Task B's R14 */
    ... (all 15 GPRs restored with Task B's values) ...
    mov rax, [rsp + TF_RAX]        /* Task B's RAX */
```

All CPU registers now hold Task B's values.

#### Step 11: Pop trap_frame, check swapgs, iretq

```asm
    add rsp, TF_SIZE               /* pop trap_frame */

    test byte ptr [rsp + HDR_CS], 3
    jz .Lstlx_x86_no_swapgs_back
    swapgs                         /* if returning to Ring 3 (Task B was lowered) */
    lfence

.Lstlx_x86_no_swapgs_back:
    add rsp, 16                    /* pop vector + error_code */
    iretq                          /* pop RIP, CS, RFLAGS, RSP, SS from iret frame */
```

`iretq` loads:
- RIP = Task B's instruction pointer → execution jumps to where B was when it yielded
- CS = Task B's code segment → correct privilege level (Ring 0 or Ring 3)
- RFLAGS = Task B's flags → interrupt enable state restored
- RSP = Task B's stack pointer → B's stack restored
- SS = Task B's stack segment

**Task B is now running.** It resumes at the exact instruction where it previously yielded, with all registers, stack, and privilege level restored.

### Step-by-Step Trace: AArch64

Same concept, different instructions. Task A (elevated, EL1 with SP_EL0) yields.

#### Step 1-3: SVC entry

```
Task A executes:  svc #SYS_YIELD
```

Exception taken at EL1 (Current EL with SP0 vector). Entry macro `STLX_A64_EL1_SP0_ENTRY`:

```asm
    STLX_A64_SAVE_GPRS              /* save x0-x30 to trap_frame on SP_EL1 */
    mrs x16, sp_el0
    str x16, [sp, #TF_SP]           /* save Task A's stack pointer (SP_EL0) */
    STLX_A64_CAPTURE_SYSREGS        /* save ELR_EL1, SPSR_EL1, ESR_EL1, FAR_EL1 */
    STLX_A64_CALL_C handler         /* x0 = sp (trap_frame*), bl handler */
```

Trap frame on SP_EL1 now has Task A's complete state: all 31 GPRs, SP, ELR (return PC), SPSR (privilege + flags), ESR, FAR.

#### Step 4: Handler calls schedule(tf)

Same as x86: `schedule(tf)` copies Task A's trap_frame to `prev->saved_frame`, overwrites `*tf` with Task B's `saved_frame`.

#### Step 5: Exit path

```asm
    ldr x16, [sp, #TF_SP]
    msr sp_el0, x16                  /* SP_EL0 = Task B's stack pointer */
    STLX_A64_WRITEBACK_SYSREGS      /* ELR_EL1, SPSR_EL1 = Task B's PC and PSTATE */
    isb
    STLX_A64_RESTORE_GPRS           /* x0-x30 = Task B's registers */
    eret                             /* resume Task B at ELR with SPSR's privilege level */
```

Task B resumes. `eret` restores the privilege level from SPSR (EL0 if B was lowered, EL1t if elevated).

### What About a Brand New Task?

A new task has never been interrupted — it has no `saved_frame` from a previous yield. We just **construct one** in the task struct:

```cpp
task* sched::create_kernel_task(void (*entry)(void*), void* arg, const char* name) {
    task* t = /* allocate */;

    // Build a trap_frame that, when restored by the exit path,
    // causes the CPU to start executing `entry(arg)`.

    // x86_64:
    t->saved_frame = {};                              // zero everything
    t->saved_frame.rip = (uint64_t)entry;             // start here
    t->saved_frame.rdi = (uint64_t)arg;               // first argument (SysV ABI)
    t->saved_frame.cs = KERNEL_CS;                    // Ring 0
    t->saved_frame.ss = KERNEL_DS;
    t->saved_frame.rflags = RFLAGS_IF;                // interrupts enabled
    t->saved_frame.rsp = task_stack_top;              // task's own stack

    // AArch64:
    t->saved_frame = {};
    t->saved_frame.elr = (uint64_t)entry;             // start here
    t->saved_frame.x[0] = (uint64_t)arg;              // first argument (AAPCS)
    t->saved_frame.spsr = SPSR_EL1T;                  // EL1 with SP_EL0 (elevated kernel task)
    t->saved_frame.sp = task_stack_top;               // task's own stack

    return t;
}
```

When `schedule()` picks this task:
1. Copies `t->saved_frame` into the trap_frame on the stack
2. Exit path restores all registers from the trap_frame
3. `iretq`/`eret` loads RIP/ELR = `entry`, RSP/SP = `task_stack_top`, RDI/x0 = `arg`
4. CPU starts executing `entry(arg)` with the given stack

No trampoline. No synthetic callee-saved frame. Just a struct with the right values.

**One thing to handle:** when `entry()` returns, we need to call `sched::exit()`. Two clean options:

**(a)** The task's entry function must call `sched::exit()` explicitly. Simplest contract.

**(b)** Set up the task stack so that the return address points to a small `task_exit_stub`:
```cpp
// Place the exit stub's address at the top of the task's stack as a return address.
uintptr_t* stack_slot = (uintptr_t*)(task_stack_top - sizeof(uintptr_t));
*stack_slot = (uintptr_t)sched_task_exit_stub;
t->saved_frame.rsp = (uint64_t)stack_slot;  // RSP points to the return address

// The stub:
extern "C" [[noreturn]] void sched_task_exit_stub() {
    sched::exit();
}
```

When `entry()` does `ret`, it pops the return address → `sched_task_exit_stub` → `sched::exit()`. This is not a trampoline — it's a safety net. The stub is a one-line function.

### The `saved_frame` in the Task Struct

Each task needs a `trap_frame` stored in its struct. This is the arch-specific trap frame that the trap entry/exit code already uses. Since the trap_frame layout differs per architecture, we use the existing types:

```cpp
// In the task struct (common code):
#if defined(__x86_64__)
    using arch_trap_frame = x86::trap_frame;
#elif defined(__aarch64__)
    using arch_trap_frame = aarch64::trap_frame;
#endif
```

Wait — this violates the "no `#ifdef` in common code" rule. Better approach: define a common `trap_frame` type in the arch headers that the common code includes via the build system's `-I` path:

```cpp
// kernel/arch/x86_64/trap/trap_frame.h already defines x86::trap_frame
// kernel/arch/aarch64/trap/trap_frame.h already defines aarch64::trap_frame
//
// We can add a common alias in each arch directory:
// kernel/arch/x86_64/sched/saved_state.h:
namespace sched {
    using saved_state = x86::trap_frame;
}

// kernel/arch/aarch64/sched/saved_state.h:
namespace sched {
    using saved_state = aarch64::trap_frame;
}
```

Common code includes `sched/saved_state.h` — the build system picks the right arch directory.

The task struct:
```cpp
struct task {
    task_exec_core exec;
    saved_state saved_frame;    // full CPU state, saved when task is not running
    uint32_t tid;
    process* proc;
    task_state state;
    uint8_t priority;
    char name[TASK_NAME_MAX];
    list_node runq_node;
    list_node proc_node;
};
```

---

## Part 2: Scheduling Policy — Exploring the Options

Let's discuss this separately from the context switch. The question is: how do we abstract the "pick the next task" logic so we can swap algorithms cleanly?

### What the Policy Actually Does

The policy manages a collection of READY tasks and answers queries. Its operations:

1. **enqueue(t)** — A task became READY; add it to the collection.
2. **dequeue(t)** — A task is no longer READY (blocked, dead, migrated); remove it.
3. **pick_next()** — Select the best task to run. Remove and return it.
4. **tick(t)** — Timer fired while `t` is running. Update accounting. (Future.)

The policy ALSO owns whatever data structure it needs (a list for round-robin, a tree for CFS, an array of queues for MLFQ).

### Option A: Function Pointer Table

```cpp
struct sched_policy {
    const char* name;
    void  (*init)(void* data, size_t data_size);
    void  (*enqueue)(void* data, task* t);
    void  (*dequeue)(void* data, task* t);
    task* (*pick_next)(void* data);
    void  (*tick)(void* data, task* t);
};
```

Runqueue has `sched_policy* policy` and `uint8_t policy_data[N]`.

**Pros:** Runtime swappable. Simple. Proven (Linux uses this pattern).
**Cons:** `void*` for data — no type safety. Opaque byte array — size must be guessed. Indirect calls — not inlineable. Feels like C, not C++.

### Option B: Abstract Base Class (Virtual Functions)

```cpp
struct sched_policy {
    virtual void enqueue(task* t) = 0;
    virtual void dequeue(task* t) = 0;
    virtual task* pick_next() = 0;
    virtual void tick(task* t) = 0;
    virtual ~sched_policy() = default;
};

struct round_robin final : sched_policy {
    list_node head;     // policy-specific data lives in the derived class

    void enqueue(task* t) override { list_add_tail(&t->runq_node, &head); }
    void dequeue(task* t) override { list_remove(&t->runq_node); }
    task* pick_next() override { /* ... */ }
    void tick(task* t) override { (void)t; }
};
```

Runqueue has `sched_policy* policy` pointing to a `round_robin` instance.

**Pros:** Runtime swappable. Type-safe — each derived class owns its data. Familiar C++ pattern. Compiler checks the interface (`override` catches typos).
**Cons:** Virtual dispatch is an indirect call (marginal overhead on the scheduler hot path). Need to allocate the derived object somewhere (static global, or heap). Virtual destructor — safe with `-fno-rtti` but need to be mindful. vtable pointer adds 8 bytes to each instance.

**But:** In a freestanding kernel with `-fno-rtti -fno-exceptions`, virtual functions actually work fine. The compiler generates a vtable (just a static array of function pointers). `dynamic_cast` doesn't work (no RTTI), but we don't need it. The destructor can be trivial. The overhead of virtual dispatch (one extra indirection per call) is negligible — `schedule()` runs at most once per timer tick or yield.

### Option C: Templates (Compile-Time Polymorphism)

```cpp
struct round_robin {
    struct rq_data { list_node head; };

    static constexpr const char* name = "round-robin";
    static void init(rq_data& d) { list_init(&d.head); }
    static void enqueue(rq_data& d, task* t) { list_add_tail(&t->runq_node, &d.head); }
    static void dequeue(rq_data& d, task* t) { list_remove(&t->runq_node); }
    static task* pick_next(rq_data& d) { /* ... */ }
    static void tick(rq_data& d, task* t) { (void)d; (void)t; }
};

template<typename Policy>
struct runqueue {
    sync::spinlock lock;
    uint32_t nr_running;
    task* idle_task;
    typename Policy::rq_data data;   // type-safe, no void*
};
```

**Pros:** Zero overhead (all calls are direct, inlineable). Fully type-safe. No vtable. Policy data is a proper typed struct. Compile-time errors if the interface isn't satisfied.
**Cons:** Not runtime-swappable (policy is baked in at compile time). The template parameter propagates into the runqueue type, which means the per-CPU variable type depends on the policy. Swapping policies requires rebuild — but for a kernel, that's normal and expected.

### Option D: CRTP (Curiously Recurring Template Pattern)

```cpp
template<typename Derived>
struct sched_policy_base {
    void enqueue(task* t) { static_cast<Derived*>(this)->enqueue_impl(t); }
    task* pick_next() { return static_cast<Derived*>(this)->pick_next_impl(); }
    // ...
};

struct round_robin : sched_policy_base<round_robin> {
    list_node head;
    void enqueue_impl(task* t) { list_add_tail(&t->runq_node, &head); }
    task* pick_next_impl() { /* ... */ }
};
```

**Pros:** Zero overhead (static dispatch, inlineable). Provides a "base class" feel without virtual functions.
**Cons:** More complex syntax. Doesn't add much over Option C (plain templates). CRTP is useful when you want a common base with shared behavior — but the scheduler core IS the shared behavior, and it's in `sched.cpp`, not in a base class. CRTP would be the wrong tool here.

### Comparison

| | Function Ptrs | Virtual | Templates | CRTP |
|---|---|---|---|---|
| Runtime swap | Yes | Yes | No (rebuild) | No (rebuild) |
| Type safety | No (`void*`) | Yes | Yes | Yes |
| Overhead | Indirect call | Indirect call | Zero | Zero |
| Code complexity | Low | Low | Low | Medium |
| Data ownership | Opaque bytes | In derived class | Typed nested struct | In derived class |
| Idiomatic C++ | No | Yes | Yes | Somewhat |
| Works with `-fno-rtti` | Yes | Yes | Yes | Yes |

### Discussion

**Do we need runtime swapping?** For a kernel, almost certainly no. Linux's scheduling classes are selected at build time (you compile with or without RT, DEADLINE, etc.). The "hot swap" you described earlier — same API, different algorithm — is achieved with compile-time selection too. You change one line in the source and rebuild. The public API (`sched::yield()`, `sched::schedule()`) stays the same regardless.

**Option B (virtual) vs Option C (templates)?** Both are clean C++. Option B is more familiar OOP. Option C is more modern C++ / zero-cost abstraction. For a kernel where every cycle counts on the scheduler path, Option C's zero overhead is appealing. But the virtual dispatch overhead is truly negligible (one memory load + indirect jump) — it's dwarfed by the cost of actually switching tasks.

**My honest recommendation:** Either **B** or **C** would work well. They're both clean, both type-safe, both work with `-fno-rtti -fno-exceptions`. I lean toward **C (templates)** because it matches the existing codebase style (templates are used in `heap.h`, `percpu.h`, etc.) and gives zero overhead. But if you prefer the OOP feel of virtual functions, **B** is perfectly fine.

The choice depends on what feels right to you for this codebase.

---

## Part 3: The Full Picture — How It All Fits Together

### Data Structures

```cpp
// ---- task_state ----
enum class task_state : uint8_t {
    created, ready, running, blocked, dead
};

// ---- process ----
struct process {
    uint32_t pid;
    uint32_t thread_count;
    list_node task_list;
    sync::spinlock lock;
};

// ---- task ----
struct task {
    task_exec_core exec;            // existing — flags, cpu, stacks, cpu_ctx
    saved_state saved_frame;        // full trap_frame, saved when not running

    uint32_t tid;
    process* proc;
    task_state state;
    uint8_t priority;               // reserved for future policies
    char name[TASK_NAME_MAX];

    list_node runq_node;            // for the runqueue (policy manages this)
    list_node proc_node;            // for the process's task list
};

// ---- runqueue (assuming template approach) ----
template<typename Policy>
struct runqueue {
    sync::spinlock lock;
    uint32_t nr_running;
    task* idle_task;
    typename Policy::rq_data data;
};
```

### Scheduler Core (Public API)

```cpp
namespace sched {
    int32_t init();
    task* create_kernel_task(void (*entry)(void*), void* arg, const char* name);
    void enqueue(task* t);
    void yield();                   // triggers software interrupt
    void schedule(trap_frame* tf);  // called by trap handler — THE context switch
    void reschedule();              // future timer hook (stub)
    [[noreturn]] void exit();
    task* current();
}
```

`yield()` is the only function tasks call directly. Everything else is internal to the scheduler or called during initialization.

### Scheduler Core (Implementation Sketch)

```cpp
// Policy selection (change this one line to swap algorithms):
using policy_t = round_robin;

DEFINE_PER_CPU(runqueue<policy_t>, cpu_rq);

void sched::schedule(trap_frame* tf) {
    auto& rq = this_cpu(cpu_rq);
    auto irq = sync::spin_lock_irqsave(rq.lock);

    task* prev = current();
    
    // If prev is still running (voluntary yield), put it back on the runqueue.
    if (prev->state == task_state::running) {
        prev->state = task_state::ready;
        policy_t::enqueue(rq.data, prev);
        rq.nr_running++;
    }
    
    // Ask the policy for the next task.
    task* next = policy_t::pick_next(rq.data);
    if (next) {
        rq.nr_running--;
    } else {
        next = rq.idle_task;
    }
    
    sync::spin_unlock_irqrestore(rq.lock, irq);

    if (prev == next) return;
    
    // THE CONTEXT SWITCH: save prev, load next.
    prev->saved_frame = *tf;
    *tf = next->saved_frame;
    
    next->state = task_state::running;
    this_cpu(current_task) = &next->exec;
}
```

That's the entire scheduler core. The complexity is in the policy (which, for round-robin, is 4 lines of code).

### Round-Robin Policy

```cpp
struct round_robin {
    static constexpr const char* name = "round-robin";

    struct rq_data {
        list_node head;
    };

    static void init(rq_data& d) {
        list_init(&d.head);
    }

    static void enqueue(rq_data& d, task* t) {
        list_add_tail(&t->runq_node, &d.head);
    }

    static void dequeue(rq_data& d, task* t) {
        (void)d;
        list_remove(&t->runq_node);
    }

    static task* pick_next(rq_data& d) {
        list_node* n = list_pop_front(&d.head);
        if (!n) return nullptr;
        return list_entry<task>(n, &task::runq_node);
    }

    static void tick(rq_data& d, task* t) {
        (void)d; (void)t;
    }
};
```

### Flow Summary

```
Task A running
    │
    ▼
sched::yield()
    │  asm: int SCHED_VECTOR / svc SYS_YIELD
    ▼
Trap Entry (existing code, unchanged)
    │  saves ALL registers to trap_frame on stack
    ▼
stlx_trap_handler(trap_frame* tf)
    │  if (vector == SCHED_VECTOR):
    ▼
sched::schedule(tf)
    │  prev->saved_frame = *tf      ← save Task A
    │  *tf = next->saved_frame      ← load Task B
    │  update current_task
    ▼
Trap Exit (existing code, unchanged)
    │  restores ALL registers from trap_frame (now Task B's values)
    │  iretq / eret
    ▼
Task B running (resumes exactly where it yielded)
```

---

## Part 4: Open Questions

### Q1: `saved_frame` type across architectures

The `saved_frame` is an arch-specific `trap_frame`. To avoid `#ifdef` in common code, we need a common type alias. Proposed approach: each arch directory provides `sched/saved_state.h` with `using saved_state = arch::trap_frame;`. Common code includes it via the build system's `-I` path. Does this feel right?

### Q2: Scheduling policy approach

Templates (Option C) or virtual functions (Option B)? Both are clean. Templates match the existing codebase style and have zero overhead. Virtual functions are more familiar OOP. Which do you prefer?

### Q3: `schedule()` signature

Should `schedule()` take `trap_frame*` directly? This means the public header needs to know about `trap_frame` (or a forward declaration). Alternative: make `schedule()` an internal function called only from the trap handler, and don't expose it in `sched.h` at all. External code only calls `yield()`, `enqueue()`, `exit()`.

### Q4: Per-task vs per-CPU system stacks

For cooperative scheduling, the system stack where the trap_frame lives is effectively per-CPU (reused for each yield). Do we need per-task system stacks for Phase 1? They're needed for future preemptive scheduling with user-mode tasks (TSS.RSP0 must point to the right stack). For now, the boot stack / GDT kernel stack works.

### Q5: Process struct — now or later?

Keep the lightweight `process` struct as proposed? Or defer until address spaces arrive?

---

## Part 5: File Layout

```
kernel/sched/
├── task_exec_core.h        (existing, unchanged)
├── thread_cpu_context.h    (existing, unchanged)
├── list.h                  (NEW — intrusive linked list)
├── task.h                  (NEW — task, process, task_state, saved_state alias)
├── sched.h                 (NEW — public API: init, yield, enqueue, exit, current)
├── sched.cpp               (NEW — schedule(), policy dispatch, task creation)
└── round_robin.h           (NEW — round-robin policy)

kernel/arch/x86_64/sched/
├── sched.cpp               (existing — boot task init; add new task frame setup)
├── saved_state.h           (NEW — using saved_state = x86::trap_frame)
└── thread_cpu_context.h    (existing)

kernel/arch/aarch64/sched/
├── sched.cpp               (existing — boot task init; add new task frame setup)
├── saved_state.h           (NEW — using saved_state = aarch64::trap_frame)
└── thread_cpu_context.h    (existing)
```

No new assembly files needed for the context switch itself. The existing `entry.S` and `vectors.S` are unchanged except for adding the schedule vector dispatch in the C handlers.

---

*This document traces through every step of the context switch with no hand-waving. The design is intentionally minimal — the existing trap infrastructure does the heavy lifting.*
