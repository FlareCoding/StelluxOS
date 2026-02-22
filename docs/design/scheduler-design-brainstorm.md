# Stellux 3.0 Scheduler Design

> **Status:** Design finalized — ready for implementation.
> **Scope:** Scheduler core (structures, states, functions, context switch). No timer. Cooperative scheduling as the initial mode. Designed so that a future timer ISR calling `sched::reschedule()` enables preemption with zero structural changes. Multi-core aware from day one.

---

## 1. Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Privilege × scheduling | **Orthogonal** — scheduler is privilege-blind | Privilege is task execution state, not scheduler input. Context switch saves/restores it transparently. |
| Task model | **Flat tasks with `process` struct grouping** | Each thread is an independently schedulable `task`. Tasks point to a `process` for identity/resource grouping. Scheduler treats every task identically. |
| `task_exec_core` | **Composition** | `task` wraps `task_exec_core`. Execution core remains the arch boundary; scheduling metadata lives in the wrapper. |
| Initial algorithm | **Simple round-robin** | No priority-based scheduling yet. Pure FIFO round-robin. Priority field exists in the struct (one byte, zero cost) for future use, but the algorithm ignores it. When timers arrive, we choose between priority, MLFQ, or CFS with real workload data. |
| Context switch model | **Callee-saved only (Linux model)** | One `context_switch()` per architecture, saves callee-saved registers only. Same function for voluntary and involuntary switches. Interrupt entry/exit handles full register save via `pt_regs` on the kernel stack — the scheduler never needs to. |
| Runqueue topology | **Per-CPU runqueues from day one** | Even with one active CPU, each CPU has its own runqueue. `schedule()` operates on the local CPU's queue. When AP cores come online, each gets its own runqueue without refactoring. |
| Scheduling mode | **Cooperative now, preemptive-ready** | `sched::yield()` calls `sched::schedule()`. Future timer ISR calls `sched::reschedule()` (stub today). No structural changes needed. |
| Timer | **Not in scope** | Scheduler exposes hooks; timer subsystem is separate. |
| Process model | **Lightweight `process` struct from day one** | Tasks hold `process*`. Process struct starts minimal but establishes the relationship now, preventing structural refactors when address spaces, file tables, or signals arrive. |
| Intrusive list | **Reusable `list_node`/`list_head`** | Small utility type used for runqueues, process task lists, future wait queues. Circular doubly-linked, Linux `list_head` pattern. |
| Task ID allocation | **Monotonic counter** | `next_tid++`, `next_pid++`. No reuse. Sufficient for early kernel. |

---

## 2. Existing Infrastructure

The scheduler builds on:

- **`task_exec_core`** — flags, CPU, dual stacks, arch CPU context
- **`thread_cpu_context`** — arch-specific register save (x86_64: all GPRs; AArch64: x0-x30, SP, PC, PSTATE). Used by interrupt entry/exit for full saves. The scheduler's context switch uses a *smaller* callee-saved subset.
- **Per-CPU `current_task`** — pointer to running task on each CPU
- **`TASK_FLAG_*`** — `RUNNING`, `PREEMPTIBLE`, `IDLE`, etc.
- **Ticket spinlocks** — `spin_lock_irqsave` / `spin_unlock_irqrestore`
- **VMM `alloc_stack()`** — stacks with guard pages for new tasks
- **Per-CPU infrastructure** — `this_cpu()`, per-CPU variables, cacheline alignment

---

## 3. Data Structures

### 3.1 Intrusive List (`list_node` / `list_head`)

Circular doubly-linked intrusive list, following the Linux `list_head` pattern. Used by runqueues, process task lists, and future wait queues.

```cpp
struct list_node {
    list_node* next;
    list_node* prev;
};
```

Operations: `list_init()`, `list_add_tail()`, `list_remove()`, `list_empty()`, `list_entry()` (container_of macro to get the enclosing struct).

### 3.2 The `process` Struct

Minimal now. Grows as subsystems are added.

```
process
├── pid             : uint32_t          (unique process ID)
├── thread_count    : uint32_t          (number of live tasks in this process)
├── task_list       : list_node         (head of intrusive list of all tasks)
├── lock            : spinlock          (protects task_list and thread_count)
└── (future: address_space*, file_table*, cwd, signal state, ...)
```

The first task created in a process has `task.tid == process.pid`. Subsequent threads in the same process share the process pointer and get unique TIDs.

### 3.3 The `task` Struct

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
├── priority                : uint8_t           (for future use — ignored by round-robin)
│
├── runq_node               : list_node         (links task into its CPU's runqueue)
├── proc_node               : list_node         (links task into its process's task list)
└── (future: wait_node for wait queues, timeslice fields, runtime accounting)
```

### 3.4 Task States

```
                ┌──────────┐
                │ CREATED  │  Allocated, not on any runqueue
                └────┬─────┘
                     │ sched::enqueue()
                     ▼
                ┌──────────┐    sched::schedule()    ┌──────────┐
            ┌──▶│  READY   │ ◄─────────────────────▶ │ RUNNING  │
            │   └──────────┘    sched::yield() /     └────┬─────┘
            │        ▲          reschedule()              │
            │        │                                    │
            │        │ sched::wake()                      │ sched::block()
            │        │                                    ▼
            │   ┌──────────┐                         ┌──────────┐
            │   │  (READY) │ ◄───────────────────────│ BLOCKED  │
            │   └──────────┘                         └──────────┘
            │
            │   ┌──────────┐
            └───│   DEAD   │  Exited — pending cleanup
                └──────────┘
```

- **CREATED** — struct initialized, not visible to scheduler
- **READY** — on a runqueue, eligible for `schedule()` to pick
- **RUNNING** — executing on a CPU; `current_task` on that CPU points here
- **BLOCKED** — waiting for an event; off all runqueues (future)
- **DEAD** — exited; not schedulable; resources await cleanup

### 3.5 Per-CPU Runqueue

```
runqueue (one per CPU)
├── lock            : spinlock
├── nr_running      : uint32_t          (count of READY tasks)
├── task_list       : list_node         (FIFO circular list of READY tasks)
└── idle_task       : task*             (fallback when queue is empty)
```

Round-robin: `schedule()` picks the first task from `task_list` (head). Yielding tasks go to the tail. O(1) enqueue and dequeue.

When SMP is active, each CPU's runqueue is independent. Load balancing (future) migrates tasks between runqueues.

---

## 4. Scheduler Core API

```cpp
namespace sched {

    // --- Initialization ---
    int32_t init();

    // --- Task/Process Creation ---

    // Create a new single-threaded kernel process.
    // Allocates a process struct + its initial task. Task starts in CREATED state.
    // Call enqueue() to make it schedulable.
    task* create_kernel_task(void (*entry)(void*), void* arg,
                            const char* name);

    // --- Enqueueing ---

    // Move a CREATED task to READY, place on its CPU's runqueue.
    void enqueue(task* t);

    // --- Core Scheduling ---

    // Voluntarily yield. Current task → READY (back of runqueue), pick next.
    void yield();

    // Core: pick next READY task from local runqueue, context switch.
    // Called by yield() and future reschedule().
    void schedule();

    // Hook for timer ISR (future). Stub/no-op today.
    void reschedule();

    // --- Task Termination ---
    [[noreturn]] void exit();

    // --- Blocking (future, state machine supports it now) ---
    // void block();
    // void wake(task* t);

    // --- Queries ---
    task* current();

} // namespace sched
```

### 4.1 `schedule()` Pseudocode

```
schedule():
    rq = this_cpu_runqueue()
    irq_flags = spin_lock_irqsave(rq->lock)

    prev = current_task on this CPU
    next = pick_next(rq)              // dequeue head of rq->task_list

    if next == nullptr:
        next = rq->idle_task          // nothing ready — run idle

    if next == prev:
        spin_unlock_irqrestore(rq->lock, irq_flags)
        return

    if prev->state == RUNNING:        // yielded, not blocked/dead
        prev->state = READY
        enqueue_locked(rq, prev)      // add to tail

    next->state = RUNNING
    next->exec.cpu = this_cpu_id()
    set this_cpu(current_task) = &next->exec

    spin_unlock_irqrestore(rq->lock, irq_flags)

    context_switch(&prev->exec, &next->exec)    // arch-specific, callee-saved only
    // prev resumes here when re-scheduled
```

### 4.2 `reschedule()` — Future Timer Hook

```
reschedule():                           // called from timer ISR
    t = sched::current()
    if !(t->exec.flags & TASK_FLAG_PREEMPTIBLE):
        return

    // future: decrement timeslice, check expiry
    schedule()
```

Today this is a stub. When timers arrive, it becomes the preemption entry point. The key point: **`schedule()` is the same function regardless of who calls it.** `yield()` calls it. `reschedule()` calls it. `block()` will call it. One function, one code path.

---

## 5. Context Switch

### 5.1 The Linux Model (What We're Following)

Linux uses a single `__switch_to_asm()` that saves only **callee-saved registers**. This works for both voluntary and involuntary switches because:

1. **Voluntary (`yield()`):** Task calls `yield()` → `schedule()` → `context_switch()`. The C calling convention means caller-saved registers are already handled. Only callee-saved need saving.

2. **Involuntary (timer ISR, future):** Interrupt entry pushes full `pt_regs` on the kernel stack. Handler calls into C code → `schedule()` → `context_switch()`. By this point, we're in a regular function call chain — callee-saved is sufficient. When the task resumes, it returns through the C chain back to the interrupt handler, which restores `pt_regs` and does `iret`/`eret`.

**Result: one `context_switch()` function per architecture. No special cases.**

### 5.2 Registers Saved by `context_switch()`

**x86_64 callee-saved:**
- RBX, RBP, R12, R13, R14, R15
- RSP (saved to/restored from `task_exec_core`)
- Return address (on the stack — `context_switch` returns to its caller)

**AArch64 callee-saved:**
- x19-x28, x29 (FP), x30 (LR)
- SP (saved to/restored from `task_exec_core`)

### 5.3 `context_switch()` Pseudocode (x86_64)

```asm
context_switch(prev_exec_core, next_exec_core):
    // Save callee-saved registers of prev
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    // Save prev's stack pointer
    mov [rdi + TASK_STACK_OFFSET], rsp

    // Restore next's stack pointer
    mov rsp, [rsi + TASK_STACK_OFFSET]

    // Restore callee-saved registers of next
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret     // returns into whatever next was doing when it was switched out
```

AArch64 is analogous with `stp`/`ldp` for register pairs and `mov sp, xN` for stack switch.

### 5.4 New Task Initial Stack Setup

When `create_kernel_task()` sets up a new task, it needs to prepare the task's stack so that the first `context_switch()` to it "returns" into the task's entry function:

1. Allocate task stack via `vmm::alloc_stack()`
2. Push a fake stack frame: callee-saved registers (all zero) + a return address pointing to a `task_entry_trampoline`
3. The trampoline calls the task's `entry(arg)` function, and if it returns, calls `sched::exit()`
4. Set `task_exec_core.task_stack_top` to the prepared stack pointer

This way, when `context_switch()` switches to the new task for the first time, it pops the fake callee-saved registers and `ret`s into the trampoline → entry function.

### 5.5 Stack Sizing

| Stack | Usable Pages | Guard Pages | Size |
|-------|-------------|-------------|------|
| Task stack | 4 | 1 | 16 KB usable |
| System stack | 4 | 1 | 16 KB usable |

---

## 6. Idle Task

- Each CPU has one idle task, created during `sched::init()`
- The boot task (already created in `arch::early_init()`) becomes CPU 0's idle task
- The idle task is **never on the runqueue** — it's the fallback when the runqueue is empty
- `schedule()` returns the idle task when `pick_next()` finds nothing
- Idle task body: `while (true) { cpu::halt(); }` — wait for interrupts

---

## 7. SMP Readiness

The design is multi-core ready from day one:

| Aspect | Design | Single-CPU Behavior | SMP Behavior |
|--------|--------|---------------------|--------------|
| Runqueues | Per-CPU | One runqueue, one CPU uses it | Each CPU has its own runqueue |
| `schedule()` | Operates on local CPU's runqueue | Picks from the only runqueue | Picks from this CPU's runqueue |
| `current_task` | Per-CPU (already exists) | One active `current_task` | Each CPU has its own |
| Runqueue lock | Per-runqueue spinlock | No contention | Locks are per-CPU, no cross-CPU contention for scheduling |
| Task creation | Enqueues to a target CPU's runqueue | Always CPU 0 | Can target any CPU |
| Load balancing | Future | N/A | Periodic migration between runqueues |
| Cross-CPU wake | Future | N/A | IPI to notify target CPU |

When AP cores come online:
1. Each AP calls `sched::init_cpu()` to set up its per-CPU runqueue and idle task
2. Tasks can be created targeting any CPU
3. `schedule()` on each CPU is independent — no changes needed
4. Load balancing is additive — a periodic check that migrates tasks between unbalanced runqueues

---

## 8. File Layout

```
kernel/sched/
├── task_exec_core.h        (existing — execution core, untouched)
├── thread_cpu_context.h    (existing — common arch context include)
├── task.h                  (NEW — task struct, process struct, task states)
├── sched.h                 (NEW — scheduler public API)
├── sched.cpp               (NEW — schedule(), yield(), exit(), runqueue)
└── list.h                  (NEW — intrusive doubly-linked list utility)

kernel/arch/x86_64/sched/
├── sched.cpp               (existing — boot task init)
├── context_switch.S        (NEW — callee-saved save/restore + stack switch)
└── thread_cpu_context.h    (existing)

kernel/arch/aarch64/sched/
├── sched.cpp               (existing — boot task init)
├── context_switch.S        (NEW — callee-saved save/restore + stack switch)
└── thread_cpu_context.h    (existing)
```

---

## 9. Phased Roadmap

### Phase 1: Scheduler Core (THIS PHASE)
- `list_node`/`list_head` intrusive list utility
- `process` struct (minimal)
- `task` struct wrapping `task_exec_core`
- Per-CPU runqueue
- `sched::init()`, `sched::create_kernel_task()`, `sched::enqueue()`
- `sched::yield()`, `sched::schedule()`, `sched::exit()`
- `sched::reschedule()` stub
- `context_switch()` on x86_64 and AArch64 (callee-saved only)
- Idle task per CPU
- Boot creates idle + test tasks, cooperative round-robin switching
- **Deliverable:** Tasks run and voluntarily context-switch on both architectures

### Phase 2: Timer + Preemption
- Timer abstraction (LAPIC / Generic Timer)
- Timer ISR calls `sched::reschedule()`
- Timeslice management (fields already in struct, algorithm plugs in)
- **Deliverable:** Preemption works; zero scheduler structural changes

### Phase 3: Blocking + Synchronization
- `sched::block()` / `sched::wake()`
- Wait queues (reusing `list_node`)
- Sleeping mutexes / semaphores
- **Deliverable:** Tasks block on events and are woken

### Phase 4: SMP Scheduling
- AP core bringup calls `sched::init_cpu()`
- Load balancing between per-CPU runqueues
- CPU affinity, migration
- IPI-based cross-CPU wakeup
- **Deliverable:** Scheduler scales across CPUs

### Phase 5: Process Model + Userspace
- Address spaces in `process`
- `fork()` / `exec()` (or Stellux equivalents)
- Multi-threaded processes (threads sharing process resources)
- File descriptor table, signals
- **Deliverable:** Full process/thread model

### Phase 6: Policy Evolution
- Evaluate MLFQ, CFS, deadline scheduling based on real workloads
- Priority field becomes active
- Scheduling classes if warranted
- **Deliverable:** Scheduler policy matches workload needs

---

*Decisions are final unless revisited by explicit discussion.*
