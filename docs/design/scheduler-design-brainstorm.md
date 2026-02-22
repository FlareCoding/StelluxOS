# Stellux 3.0 Scheduler Design

> **Status:** Design finalized — ready for implementation.
> **Scope:** Scheduler core + pluggable policy. No timer. Cooperative scheduling initially. Designed so that a timer ISR enables preemption with zero structural changes. Multi-core aware from day one.

---

## 1. Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Privilege × scheduling | **Orthogonal** — scheduler is privilege-blind | Privilege is task state. Context switch saves/restores it via trap frame. |
| Task model | **Flat tasks with `process` struct** | Each thread is an independent `task`. Tasks point to a `process`. Scheduler treats all tasks identically. |
| `task_exec_core` | **Composition** | `task` wraps `task_exec_core`. |
| Initial algorithm | **Round-robin** | Pure FIFO. No priority logic. Priority field reserved for future use. |
| Context switch model | **Interrupt-routed** | `yield()` triggers a software interrupt. Both voluntary and involuntary switches go through the trap entry/exit path. `iretq`/`eret` handles privilege-level-aware returns automatically. |
| Scheduler architecture | **Core + pluggable policy** | Scheduler core (task lifecycle, context switch, `schedule()`) is separated from scheduling policy (`pick_next()`, `enqueue()`, `dequeue()`). Policy is swappable via function pointers without changing the core. |
| Runqueue topology | **Per-CPU from day one** | Each CPU has its own runqueue. SMP-ready without refactoring. |
| Process model | **Lightweight `process` struct from day one** | Tasks hold `process*`. Small upfront cost prevents future refactors. |
| Intrusive list | **Reusable `list_node`/`list_head`** | Linux `list_head` pattern. Used everywhere. |
| Task ID allocation | **Monotonic counter** | No reuse. Sufficient for now. |

---

## 2. Key Design: Interrupt-Routed Context Switch

### 2.1 The Problem with Callee-Saved-Only Switching

With dynamic privilege, a task can be running at Ring 0/EL1 (elevated) or Ring 3/EL0 (lowered) when it wants to yield. A plain function-call-based `context_switch()` returns via `ret`, which does not change privilege levels. This creates two problems:

1. A lowered task at Ring 3 can't directly call `yield()` — it's privileged code. It would need to elevate first, yield, then lower on resume. Extra steps, extra complexity.
2. The return path must somehow know whether to return to Ring 0 or Ring 3, adding a conditional branch in the context switch.

### 2.2 The Solution: Route Through the Trap Path

`yield()` triggers a **software interrupt** — a dedicated IDT vector on x86_64, or a specific SVC immediate on AArch64. This means:

1. The existing trap entry code saves the **full machine state** (trap_frame) including the privilege level (CS on x86, SPSR on AArch64)
2. The trap handler calls `sched::schedule()`
3. `schedule()` uses a callee-saved context switch to swap between tasks' system stacks
4. On return, the trap exit code restores the trap_frame and executes `iretq`/`eret`
5. `iretq`/`eret` **automatically** restores the correct privilege level from the saved CS/SPSR

### 2.3 Why This Is More Robust

The existing trap entry/exit code already handles every privilege scenario:

**x86_64 (`entry.S`):**
- From Ring 3: swapgs + save trap_frame → handler → restore trap_frame + swapgs + iretq back to Ring 3
- From Ring 0 (elevated): stack switch to system_stack + save trap_frame → handler → restore trap_frame + iretq back to Ring 0
- From Ring 0 (pure kernel): save trap_frame → handler → restore trap_frame + iretq back to Ring 0

**AArch64 (`vectors.S`):**
- From EL0 (lowered): `STLX_A64_EL0_ENTRY` — saves SP_EL0, runs handler on SP_EL1, eret restores EL0
- From EL1 with SP0 (elevated): `STLX_A64_EL1_SP0_ENTRY` — saves SP_EL0, runs handler on SP_EL1, eret restores EL1 + SP_EL0
- From EL1 with SPx (kernel): `STLX_A64_EL1_ENTRY` — saves kernel SP, runs handler, eret restores EL1

All of this is **already implemented and tested**. The interrupt-routed context switch reuses it entirely. No new privilege-handling code needed.

### 2.4 Uniform Path for All Context Switches

| Trigger | Entry | Handler | Exit |
|---------|-------|---------|------|
| `yield()` (voluntary) | Software interrupt → trap entry → trap_frame saved | `schedule()` → context_switch() | trap exit → trap_frame restored → iretq/eret |
| Timer tick (involuntary, future) | Hardware interrupt → trap entry → trap_frame saved | `reschedule()` → `schedule()` → context_switch() | trap exit → trap_frame restored → iretq/eret |

**Same entry path. Same exit path. Same context_switch(). One code path.**

### 2.5 How It Works Step by Step

**Task A yields:**
```
1. Task A (at any privilege level) executes: int SCHED_VECTOR / svc SCHED_NUM
2. Trap entry saves Task A's full state (trap_frame) on Task A's system stack
3. Trap handler calls sched::schedule()
4. schedule() picks Task B via policy->pick_next()
5. schedule() calls context_switch(&A->exec, &B->exec)
6. context_switch() saves callee-saved regs (handler context) on A's system stack
7. context_switch() switches SP to B's system stack
8. context_switch() restores callee-saved regs from B's system stack
9. context_switch() returns (via ret) into B's handler context
10. B's schedule() returns → B's trap handler returns
11. Trap exit restores B's trap_frame → iretq/eret
12. Task B resumes at whatever privilege level it was at
```

**Timer preempts Task A (future):**
```
Steps 1-12 are identical, except step 1 is a hardware interrupt instead of
a software interrupt. The rest is the same path.
```

### 2.6 New Task Bootstrap

A newly created task has never been interrupted — it has no trap_frame. We build a **synthetic stack** so that the first `context_switch()` to it follows the normal return path:

```
New task's system stack (growing downward):

    ┌─────────────────────────┐  ← original system_stack_top
    │                         │
    │  Synthetic trap_frame   │  ← full trap frame:
    │   RIP/ELR = entry_tramp │     instruction pointer → task entry trampoline
    │   CS/SPSR = privilege   │     desired privilege level (Ring 0 for kernel task)
    │   RSP/SP = task_stack   │     task's own stack (task_stack_top)
    │   RFLAGS/PSTATE = IF=1  │     interrupts enabled
    │   RDI/x0 = arg          │     first argument to entry function
    │   all other GPRs = 0    │
    │                         │
    ├─────────────────────────┤
    │                         │
    │  Fake callee-saved      │  ← callee-saved registers (all zero)
    │   return addr = trap_ret│     points to trap exit label in entry.S
    │   r12 = &trap_frame     │     (x86_64: used by trap exit to find trap_frame)
    │                         │
    └─────────────────────────┘  ← saved system_stack_top in task_exec_core
```

When `context_switch()` switches to this task:
1. Restores fake callee-saved registers
2. `ret` to `trap_return` label (the trap exit path in `entry.S` / `vectors.S`)
3. Trap exit restores the synthetic trap_frame
4. `iretq`/`eret` transitions to the entry trampoline at the desired privilege level
5. Entry trampoline calls `entry(arg)`, then calls `sched::exit()` if it returns

### 2.7 Required Changes to Trap Infrastructure

Minimal, additive changes:

1. **Reserve a vector/SVC number** for scheduling:
   - x86_64: `SCHED_VECTOR` (e.g., vector 0x81) in the IDT
   - AArch64: `SYS_YIELD` (e.g., SVC #998) dispatched in the sync handler

2. **Dispatch in the trap handler:**
   - x86_64: `if (tf->vector == SCHED_VECTOR) { sched::schedule(); return; }`
   - AArch64: already dispatched via SVC number in the existing sync handlers

3. **Export a trap exit label** for new task bootstrap:
   - x86_64: `.globl stlx_x86_trap_return` at the trap exit point in `entry.S`
   - AArch64: equivalent label in `vectors.S`

No structural changes to the existing trap entry/exit code.

---

## 3. Key Design: Scheduler Core vs Policy Separation

### 3.1 The Principle

The scheduler has two distinct concerns:

1. **Scheduler Core** — The mechanics of scheduling: task lifecycle, state transitions, context switching, runqueue management, `schedule()`, `yield()`, `exit()`. These don't change when you change the algorithm.

2. **Scheduling Policy** — The algorithm that answers one question: "given the set of ready tasks, which one runs next?" This is what changes between round-robin, priority, MLFQ, CFS, etc.

The core calls the policy through a well-defined interface. Swapping algorithms means pointing to a different policy implementation. **The core API does not change.**

### 3.2 Policy Interface

```cpp
struct sched_policy {
    const char* name;

    // Initialize policy-specific state in the runqueue.
    void (*init)(runqueue& rq);

    // Add a READY task to the runqueue.
    void (*enqueue)(runqueue& rq, task* t);

    // Remove a task from the runqueue (e.g., when it blocks or exits).
    void (*dequeue)(runqueue& rq, task* t);

    // Select the next task to run. Returns nullptr if no tasks are ready.
    // The returned task is removed from the runqueue.
    task* (*pick_next)(runqueue& rq);

    // Timer tick callback (future). Called on each tick for the running task.
    // Policy can use this for timeslice accounting, priority adjustment, etc.
    void (*tick)(runqueue& rq, task* t);
};
```

### 3.3 How the Core Uses the Policy

```cpp
// In schedule():
task* next = rq->policy->pick_next(*rq);
if (!next) next = rq->idle_task;

// In enqueue():
rq->policy->enqueue(*rq, t);
rq->nr_running++;

// In reschedule() (future):
rq->policy->tick(*rq, current);
// policy may set a "needs_resched" flag → core calls schedule()
```

The core never decides *how* to pick the next task. It just asks the policy.

### 3.4 Round-Robin Policy (Initial Implementation)

```
round_robin_policy:
    init:      initialize task_list head in rq->policy_data
    enqueue:   list_add_tail(task->runq_node, task_list)
    dequeue:   list_remove(task->runq_node)
    pick_next: list_pop_front(task_list), return owning task (or nullptr)
    tick:      no-op (no timeslice management yet)
```

Pure FIFO. O(1) enqueue, O(1) dequeue, O(1) pick.

### 3.5 Future Policy: CFS (Example of Swappability)

```
cfs_policy:
    init:      initialize rb_tree root, min_vruntime = 0
    enqueue:   insert task into rb_tree by vruntime
    dequeue:   remove task from rb_tree
    pick_next: extract leftmost node (smallest vruntime)
    tick:      update current task's vruntime, check if leftmost has less
```

To switch from round-robin to CFS:
1. Implement the `cfs_policy` struct
2. Point `rq->policy` to `&cfs_policy` during init
3. Add `vruntime` field to the task struct (or `sched_entity` sub-struct)
4. **Zero changes to the scheduler core**

### 3.6 Policy-Specific Runqueue Data

The runqueue carries opaque storage for policy-specific state:

```cpp
constexpr size_t SCHED_POLICY_DATA_SIZE = 128;

struct runqueue {
    sync::spinlock lock;
    uint32_t nr_running;
    task* idle_task;
    const sched_policy* policy;
    alignas(16) uint8_t policy_data[SCHED_POLICY_DATA_SIZE];
};
```

Each policy's `init()` function initializes `policy_data` as whatever it needs. Round-robin uses it as a `list_node` (16 bytes). CFS would use it as an rb_tree root + min_vruntime (maybe 24 bytes). The core never interprets this data.

---

## 4. Data Structures

### 4.1 Intrusive List (`list_node`)

Circular doubly-linked intrusive list (Linux `list_head` pattern):

```cpp
struct list_node {
    list_node* next;
    list_node* prev;
};
```

Operations: `list_init()`, `list_add_tail()`, `list_remove()`, `list_empty()`, `list_entry()` (container_of).

### 4.2 The `process` Struct

```
process
├── pid             : uint32_t
├── thread_count    : uint32_t
├── task_list       : list_node         (head: intrusive list of tasks in this process)
├── lock            : spinlock
└── (future: address_space*, file_table*, ...)
```

### 4.3 The `task` Struct

```
task
├── exec                    : task_exec_core    (existing — execution/arch state)
│   ├── flags               : uint32_t
│   ├── cpu                 : uint32_t
│   ├── task_stack_top      : uintptr_t
│   ├── system_stack_top    : uintptr_t
│   └── cpu_ctx             : thread_cpu_context
│
├── tid                     : uint32_t          (unique task/thread ID)
├── proc                    : process*          (owning process)
├── state                   : task_state        (CREATED, READY, RUNNING, BLOCKED, DEAD)
├── name                    : char[TASK_NAME_MAX]
│
├── priority                : uint8_t           (reserved — not used by round-robin)
│
├── runq_node               : list_node         (policy uses this for runqueue linkage)
├── proc_node               : list_node         (links task into process->task_list)
└── (future: wait_node, sched_entity with vruntime, etc.)
```

### 4.4 Task States

```
                ┌──────────┐
                │ CREATED  │  Allocated, not on any runqueue
                └────┬─────┘
                     │ sched::enqueue()
                     ▼
                ┌──────────┐   sched::schedule()    ┌──────────┐
            ┌──▶│  READY   │◄─────────────────────▶ │ RUNNING  │
            │   └──────────┘   sched::yield() /     └────┬─────┘
            │        ▲         reschedule()              │
            │        │                                   │
            │        │ sched::wake()                     │ sched::block()
            │        │                                   ▼
            │   ┌──────────┐                        ┌──────────┐
            │   │  (READY) │◄───────────────────────│ BLOCKED  │
            │   └──────────┘                        └──────────┘
            │
            │   ┌──────────┐
            └───│   DEAD   │  Exited — pending cleanup
                └──────────┘
```

### 4.5 Per-CPU Runqueue

```
runqueue (one per CPU)
├── lock            : spinlock
├── nr_running      : uint32_t
├── idle_task       : task*
├── policy          : const sched_policy*
└── policy_data     : uint8_t[128]      (opaque, policy-specific)
```

---

## 5. Scheduler Core API

```cpp
namespace sched {

    // --- Initialization ---
    int32_t init();                     // init BSP runqueue, idle task, policy

    // --- Task/Process Creation ---
    task* create_kernel_task(
        void (*entry)(void*), void* arg,
        const char* name
    );

    // --- Enqueueing ---
    void enqueue(task* t);              // CREATED → READY, add to runqueue

    // --- Core Scheduling ---
    void yield();                       // triggers software interrupt → schedule()
    void schedule();                    // pick next via policy, context_switch
    void reschedule();                  // future timer hook (stub today)

    // --- Task Termination ---
    [[noreturn]] void exit();           // current task → DEAD, schedule()

    // --- Blocking (future) ---
    // void block();                    // current task → BLOCKED, schedule()
    // void wake(task* t);             // BLOCKED → READY, enqueue

    // --- Queries ---
    task* current();                    // this CPU's running task

} // namespace sched
```

### 5.1 `yield()` Implementation

```cpp
// x86_64:
void yield() {
    asm volatile("int %0" :: "i"(SCHED_VECTOR) : "memory");
}

// AArch64:
void yield() {
    asm volatile("svc %0" :: "i"(SYS_YIELD) : "memory");
}
```

That's it. The trap path handles everything else.

### 5.2 `schedule()` Pseudocode

```
schedule():
    rq = this_cpu_runqueue()
    irq_flags = spin_lock_irqsave(rq->lock)

    prev = current_task_on_this_cpu()

    if prev->state == RUNNING:
        prev->state = READY
        rq->policy->enqueue(rq, prev)       // policy decides where to put it

    next = rq->policy->pick_next(rq)         // policy decides what runs next
    if next == nullptr:
        next = rq->idle_task

    next->state = RUNNING
    next->exec.cpu = this_cpu_id()
    set_current_task(next)

    spin_unlock_irqrestore(rq->lock, irq_flags)

    if prev != next:
        context_switch(&prev->exec, &next->exec)
```

### 5.3 `reschedule()` — Future Timer Hook

```
reschedule():                               // called from timer ISR
    rq = this_cpu_runqueue()
    t = current_task()
    if !(t->exec.flags & TASK_FLAG_PREEMPTIBLE):
        return
    rq->policy->tick(rq, t)                 // policy updates accounting
    // if policy decides preemption is needed:
    schedule()
```

The core just calls `policy->tick()`. The **policy** decides whether to preempt. For round-robin, `tick()` is a no-op. For CFS, it would update vruntime and check the tree.

---

## 6. Context Switch (Internal Mechanism)

### 6.1 Callee-Saved Switch Between System Stacks

Even though yield goes through the trap path, we still need a mechanism to switch between two tasks' system stacks. This is the callee-saved context switch — it operates on the handler's C context, not the task's user context.

**x86_64 callee-saved:** RBX, RBP, R12, R13, R14, R15, RSP (implicit), return address (on stack)
**AArch64 callee-saved:** x19-x28, x29 (FP), x30 (LR), SP

### 6.2 Flow Diagram

```
Task A's system stack                  Task B's system stack
─────────────────────                  ─────────────────────
┌───────────────────┐                  ┌───────────────────┐
│  A's trap_frame   │                  │  B's trap_frame   │
│  (saved by trap   │                  │  (saved when B    │
│   entry when A    │                  │   was switched    │
│   yielded)        │                  │   out earlier)    │
├───────────────────┤                  ├───────────────────┤
│ handler C frames  │                  │ handler C frames  │
│ (schedule, etc.)  │                  │ (schedule, etc.)  │
├───────────────────┤                  ├───────────────────┤
│ callee-saved regs │ ◄── save here   │ callee-saved regs │ ◄── restore from here
│ + return address  │                  │ + return address  │
└───────────────────┘                  └───────────────────┘
        ▲                                      ▲
        │                                      │
   A's exec.system_stack_top              B's exec.system_stack_top
   (saved during switch-out)              (restored during switch-in)
```

### 6.3 Stack Sizing

| Stack | Usable Pages | Guard Pages | Size |
|-------|-------------|-------------|------|
| Task stack | 4 | 1 | 16 KB usable |
| System stack | 4 | 1 | 16 KB usable |

---

## 7. Idle Task

- Each CPU has one idle task (boot task becomes CPU 0's idle task)
- **Never on the runqueue** — fallback when `pick_next()` returns nullptr
- Body: `while (true) { cpu::halt(); }`
- Per-CPU idle tasks are created during SMP bringup

---

## 8. SMP Readiness

| Aspect | Design | Single-CPU | SMP |
|--------|--------|------------|-----|
| Runqueues | Per-CPU | One runqueue | Each CPU has its own |
| `schedule()` | Operates on local runqueue | One runqueue | Per-CPU, independent |
| Runqueue lock | Per-runqueue spinlock | No contention | Per-CPU, minimal contention |
| Policy | Per-runqueue policy pointer | One policy instance | Same policy, per-CPU state |
| Load balancing | Future | N/A | Periodic migration |
| Cross-CPU wake | Future | N/A | IPI to target CPU |

---

## 9. File Layout

```
kernel/sched/
├── task_exec_core.h        (existing — untouched)
├── thread_cpu_context.h    (existing — untouched)
├── list.h                  (NEW — intrusive doubly-linked list)
├── task.h                  (NEW — task, process, task_state)
├── sched_policy.h          (NEW — sched_policy interface)
├── sched.h                 (NEW — scheduler core public API)
├── sched.cpp               (NEW — scheduler core: schedule(), enqueue(), exit())
├── runqueue.h              (NEW — per-CPU runqueue struct)
└── round_robin.cpp         (NEW — round-robin policy implementation)

kernel/arch/x86_64/sched/
├── sched.cpp               (existing — boot task init; extend for context_switch)
├── context_switch.S        (NEW — callee-saved save/restore + stack switch)
└── thread_cpu_context.h    (existing)

kernel/arch/aarch64/sched/
├── sched.cpp               (existing — boot task init; extend for context_switch)
├── context_switch.S        (NEW — callee-saved save/restore + stack switch)
└── thread_cpu_context.h    (existing)
```

---

## 10. Phased Roadmap

### Phase 1: Scheduler Core (THIS PHASE)
- `list_node` intrusive list utility
- `process` struct (minimal)
- `task` struct wrapping `task_exec_core`
- `sched_policy` interface + round-robin implementation
- Per-CPU runqueue
- `sched::init()`, `sched::create_kernel_task()`, `sched::enqueue()`
- `sched::yield()` via software interrupt
- `sched::schedule()` with policy dispatch
- `sched::exit()`
- `sched::reschedule()` stub
- `context_switch()` on x86_64 and AArch64
- New task synthetic stack bootstrap
- Trap handler dispatch for scheduling vector/SVC
- Trap exit label export for new task bootstrap
- Idle task per CPU
- Boot creates idle + test tasks, cooperative round-robin
- **Deliverable:** Tasks run and cooperatively switch on both architectures

### Phase 2: Timer + Preemption
- Timer abstraction (LAPIC / Generic Timer)
- Timer ISR calls `sched::reschedule()`
- Policy `tick()` implementation (round-robin timeslice, or swap to CFS/MLFQ)
- **Deliverable:** Preemption works; zero core changes

### Phase 3: Blocking + Synchronization
- `sched::block()` / `sched::wake()`
- Wait queues (reusing `list_node`)
- Sleeping mutexes
- **Deliverable:** Tasks block on events

### Phase 4: SMP
- AP bringup + per-CPU runqueue init
- Load balancing
- CPU affinity, migration, IPI wakeup
- **Deliverable:** Multi-core scheduling

### Phase 5: Process Model + Userspace
- Address spaces in `process`
- Process creation APIs
- Multi-threaded processes
- **Deliverable:** Full process/thread model

### Phase 6: Policy Evolution
- Swap round-robin for CFS, MLFQ, or priority-based
- Same core, different `sched_policy` implementation
- **Deliverable:** Production-quality scheduling

---

*Decisions are final unless revisited by explicit discussion.*
