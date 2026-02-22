# Stellux 3.0 Scheduler Design Brainstorm

> **Status:** Design exploration — refining core semantics.
> **Scope:** Scheduler core (structures, states, functions, context switch). No timer, no preemption implementation yet. Cooperative scheduling as the initial mode. Designed so that a future timer tick calling `sched::reschedule()` enables preemption with zero structural changes.

---

## 1. Decisions Made

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Privilege × scheduling | **Orthogonal** — scheduler is privilege-blind | Privilege is task execution state, not scheduler input. Context switch saves/restores it transparently. |
| Task model | **Flat tasks with process grouping** | Each thread is an independently schedulable `task`. Tasks carry a `pid` so process semantics exist, but the scheduler treats every task identically. |
| `task_exec_core` relationship | **Composition** | A new `task` struct contains `task_exec_core` plus scheduling and identity metadata. `task_exec_core` remains the arch/execution boundary. |
| Initial scheduling mode | **Cooperative** | Tasks yield voluntarily via `sched::yield()`. No timer dependency. |
| Preemption readiness | **Designed in from day one** | The `sched::reschedule()` entry point exists. Timer ISR calls it later. The runqueue and state machine support preemption without structural changes. |
| Timer | **Not part of this design** | Timer abstraction is a separate future subsystem. The scheduler exposes hooks for it. |

---

## 2. What We're Building On

Existing infrastructure the scheduler will use:

- **`task_exec_core`** — flags, CPU, dual stacks, arch CPU context
- **`thread_cpu_context`** — arch-specific register save area (x86_64: RAX-R15, RIP, RFLAGS; AArch64: x0-x30, SP, PC, PSTATE)
- **Per-CPU `current_task`** — pointer to running task on each CPU
- **`TASK_FLAG_*`** — runtime flags including `RUNNING`, `PREEMPTIBLE`, `IDLE`
- **Ticket spinlocks** — `spin_lock_irqsave` / `spin_unlock_irqrestore` for scheduler lock
- **VMM `alloc_stack()`** — allocates stacks with guard pages for new tasks
- **Per-CPU infrastructure** — `this_cpu()`, per-CPU variables

---

## 3. Task Structure

### 3.1 The `task` Struct (Composition over `task_exec_core`)

```
task
├── exec                    : task_exec_core    (existing — execution state)
│   ├── flags               : uint32_t
│   ├── cpu                 : uint32_t
│   ├── task_stack_top      : uintptr_t
│   ├── system_stack_top    : uintptr_t
│   └── cpu_ctx             : thread_cpu_context
│
├── tid                     : uint32_t          (unique task/thread ID)
├── pid                     : uint32_t          (process ID — groups threads)
├── state                   : task_state        (CREATED, READY, RUNNING, BLOCKED, DEAD)
├── name                    : char[TASK_NAME_MAX] (debug name, e.g. "init", "worker-3")
│
├── sched                   : sched_entity      (scheduling metadata)
│   ├── priority            : uint8_t           (0 = highest, N = lowest)
│   ├── timeslice_remaining : uint32_t          (for future preemption — ticks remaining)
│   ├── timeslice_default   : uint32_t          (reset value when timeslice expires)
│   └── total_runtime_us    : uint64_t          (cumulative runtime, for accounting)
│
├── runq_node               : intrusive list node (links task into runqueue)
└── (future: wait_channel, address_space*, file table*, ...)
```

### 3.2 Task States

```
                ┌──────────┐
                │ CREATED  │  Task allocated, not yet on any runqueue
                └────┬─────┘
                     │ sched::enqueue()
                     ▼
                ┌──────────┐    sched::schedule()    ┌──────────┐
                │  READY   │ ◄──────────────────────▶│ RUNNING  │
                └──────────┘    sched::yield() /     └────┬─────┘
                     ▲          reschedule()              │
                     │                                    │
                     │ sched::wake()                      │ sched::block()
                     │                                    ▼
                ┌──────────┐                         ┌──────────┐
                │  READY   │ ◄───────────────────────│ BLOCKED  │
                └──────────┘                         └──────────┘
                                                          
                ┌──────────┐
                │   DEAD   │  Task exited — resources pending cleanup
                └──────────┘
```

- **CREATED** → task struct is initialized but not visible to the scheduler
- **READY** → on a runqueue, eligible to be picked by `schedule()`
- **RUNNING** → currently executing on a CPU (`current_task` on some CPU points here)
- **BLOCKED** → waiting for an event; not on any runqueue (future: on a wait queue)
- **DEAD** → has exited; not schedulable; resources await cleanup

### 3.3 Process Grouping

For now, "process" is just a `pid` value shared by all tasks that belong to the same process. The first task in a process has `tid == pid`. Additional threads spawned within that process share the same `pid` but get unique `tid` values.

No separate `process` struct is needed yet. When address spaces, file descriptor tables, or signal handling arrive, a `process` struct can be introduced and tasks will point to it. The `pid` field already establishes the semantic grouping.

**Open question: Should there be a lightweight `process` struct now (even if nearly empty) to avoid a future refactor? Or is `pid` sufficient?**

---

## 4. Scheduler Core API

### 4.1 Proposed Public Interface

```cpp
namespace sched {

    // --- Initialization ---
    
    // Initialize the scheduler subsystem (runqueue, idle task setup).
    // Called once during boot, after mm::init().
    int32_t init();
    
    // --- Task Creation ---
    
    // Create a new kernel task. Returns task pointer or nullptr on failure.
    // The task starts in CREATED state. Call enqueue() to make it schedulable.
    // entry: function pointer the task will execute
    // arg: opaque argument passed to entry
    // name: debug name
    // priority: scheduling priority (0 = highest)
    task* create_kernel_task(void (*entry)(void*), void* arg,
                            const char* name, uint8_t priority);

    // --- Enqueueing ---
    
    // Move a CREATED task to READY and place it on the runqueue.
    // Separate from create so the caller can configure the task before it becomes visible.
    void enqueue(task* t);
    
    // --- Core Scheduling ---
    
    // Voluntarily yield the current CPU. The current task moves to READY
    // and schedule() picks the next task.
    void yield();
    
    // Select the next READY task and switch to it.
    // Called by yield(), and in the future by the timer ISR (reschedule()).
    // If no other task is ready, continues running the current task
    // (or switches to idle).
    void schedule();
    
    // Entry point for preemptive scheduling. Called from timer ISR.
    // Decrements timeslice; if expired, calls schedule().
    // Currently a no-op stub (no timer yet), but the signature exists.
    void reschedule();
    
    // --- Task Termination ---
    
    // Current task exits. Moves to DEAD state, calls schedule().
    // Does not return.
    [[noreturn]] void exit();
    
    // --- Blocking (future, but state machine supports it now) ---
    
    // Block the current task. Moves to BLOCKED state, calls schedule().
    // void block();
    
    // Wake a blocked task. Moves from BLOCKED to READY, enqueues.
    // void wake(task* t);
    
    // --- Queries ---
    
    // Get the currently running task on this CPU.
    task* current();

} // namespace sched
```

### 4.2 How `schedule()` Works (Pseudocode)

```
schedule():
    irq_save()
    lock(runqueue_lock)
    
    prev = current_task on this CPU
    next = pick_next_task()     // highest priority READY task from runqueue
    
    if next == prev:
        unlock(runqueue_lock)
        irq_restore()
        return                  // nothing to do
    
    if prev->state == RUNNING:
        prev->state = READY
        enqueue_locked(prev)    // put back on runqueue (it yielded, not blocked)
    
    next->state = RUNNING
    next->exec.cpu = this_cpu_id
    set current_task = next
    
    unlock(runqueue_lock)
    
    context_switch(&prev->exec, &next->exec)   // arch-specific
    // when prev resumes later, it returns here
    
    irq_restore()
```

### 4.3 How `reschedule()` Will Work (Future)

```
reschedule():                       // called from timer ISR
    t = current_task
    if !(t->exec.flags & TASK_FLAG_PREEMPTIBLE):
        return                      // respect non-preemptible sections
    
    t->sched.timeslice_remaining--
    if t->sched.timeslice_remaining > 0:
        return                      // still has time
    
    t->sched.timeslice_remaining = t->sched.timeslice_default
    schedule()
```

This is why the scheduler core must be correct and complete *now* — `reschedule()` is just a thin policy wrapper around `schedule()`.

---

## 5. Runqueue Design

### 5.1 Initial: Single Global Runqueue

For cooperative single-CPU scheduling, a single global runqueue is sufficient:

```
runqueue
├── lock        : spinlock          (protects the queue)
├── nr_running  : uint32_t          (number of READY tasks)
└── queue       : intrusive list    (FIFO of READY tasks, ordered by priority)
```

**Priority handling options:**
- **(a) Single list, sorted by priority** — O(n) insertion, O(1) pick. Fine for small task counts.
- **(b) Array of lists, one per priority level** — O(1) insertion, O(1) pick (scan bitmap for highest non-empty). Classic approach. Scales better.
- **(c) Start with (a), evolve to (b)** — Get it working, then optimize.

**Open question: How many priority levels? 8? 16? 32? 64?**

More levels = finer granularity but more complexity. 32 is a common choice (Linux RT uses 100, but that's overkill for now).

### 5.2 Future: Per-CPU Runqueues

When SMP arrives, each CPU gets its own runqueue. Load balancing migrates tasks between them. The `schedule()` function remains the same — it just operates on the local CPU's runqueue. This is a data structure change, not a semantic change, which is why getting the semantics right now matters.

---

## 6. Context Switch

### 6.1 Architecture Split

**Common (in `kernel/sched/`):**
- `schedule()` — picks next task, updates states, calls arch switch
- Task state management
- Runqueue operations

**Arch-specific (in `kernel/arch/*/sched/`):**
- `context_switch(task_exec_core* prev, task_exec_core* next)` — saves prev's registers, restores next's registers, switches stacks
- This is the only assembly needed for the scheduler

### 6.2 What Context Switch Saves/Restores

On both architectures, the context switch only needs to save **callee-saved registers** plus the stack pointer and instruction pointer, because the compiler already saves caller-saved registers at the call site.

**x86_64 callee-saved:** RBX, RBP, R12-R15, RSP, RIP (via return address), RFLAGS
**AArch64 callee-saved:** x19-x29, x30 (LR), SP

The full `thread_cpu_context` (all registers) is for interrupt/trap save — the *voluntary* context switch can use a smaller set. But if we want `schedule()` to work identically whether called from `yield()` (voluntary) or from `reschedule()` in a timer ISR (involuntary), we may want a uniform save format.

**Open question: Should voluntary and involuntary context switches use the same save format, or should voluntary switches be optimized to save fewer registers?**

- **Same format:** Simpler. One `context_switch()` function. No special cases.
- **Different format:** Faster voluntary switches. But two code paths to maintain on each architecture.

### 6.3 Stack Considerations

Each task has two stacks:
- `task_stack_top` — the task's own stack (user/lowered execution)
- `system_stack_top` — the system stack (for interrupts, syscalls, elevated execution)

During a voluntary `yield()`, the task is calling from its own stack. The context switch saves the stack pointer and switches to the next task's stack. When the task resumes, it returns from `context_switch()` back into `yield()` back into whatever code called `yield()`.

---

## 7. Idle Task

Each CPU needs an idle task — the task that runs when nothing else is ready. The boot task is currently marked with `TASK_FLAG_IDLE`. In the scheduler:

- The idle task is **never on the runqueue** — it's the fallback when the runqueue is empty
- `schedule()` returns the idle task when `pick_next_task()` finds nothing
- The idle task's body is `while (true) { cpu::halt(); }` — wait for interrupts
- Per-CPU: each CPU has its own idle task (already the case with per-CPU `current_task`)

---

## 8. Open Design Questions

These are the remaining questions that need answers before implementation:

### Q1: Lightweight `process` struct now, or just `pid`?

**Option A: Just `pid` on each task.** Simpler. Process grouping is implicit. When you need a process struct later, add it and have tasks point to it.

**Option B: Lightweight `process` struct now.** Even if it only contains `pid` and a task list, it establishes the pattern. New threads are added to their process's task list. Makes future features (shared address space, signals, process-wide operations) easier.

### Q2: How many priority levels?

Reasonable choices: **8**, **16**, or **32**. Fewer is simpler but less flexible. More gives finer control but is premature if we don't have policies using them yet.

### Q3: Voluntary vs involuntary context switch — same save format?

See section 6.2. One code path (simpler) vs two (faster voluntary switches)?

### Q4: Intrusive list implementation — roll our own or keep it minimal?

The runqueue and future wait queues need an intrusive linked list (no allocation to enqueue). Options:
- Minimal `prev`/`next` pointers embedded in `task` — dead simple
- A small reusable `list_node` / `list_head` type (like Linux's `list_head`) — slightly more infrastructure but reusable everywhere

### Q5: Task ID allocation strategy?

Simple monotonic counter (`next_tid++`)? Or something more structured? Monotonic is fine for now but doesn't handle TID reuse after task death. For early kernel development, monotonic is probably sufficient.

### Q6: How should `create_kernel_task` allocate the task struct and stack?

- Task struct: `kalloc_new<task>()` (privileged heap)
- Task stack: `vmm::alloc_stack()` with guard pages
- System stack: separate `vmm::alloc_stack()` for interrupt/elevated use
- Entry point setup: arch-specific — set up initial `cpu_ctx` so that `context_switch()` to this task "returns" into the entry function

---

## 9. Proposed File Layout

```
kernel/sched/
├── task_exec_core.h        (existing — execution core, unchanged)
├── thread_cpu_context.h    (existing — common include for arch context)
├── task.h                  (NEW — full task struct, states, task_id, process grouping)
├── sched.h                 (NEW — scheduler public API)
├── sched.cpp               (NEW — schedule(), yield(), exit(), runqueue logic)
├── runqueue.h              (NEW — runqueue data structure)
└── runqueue.cpp            (NEW — runqueue operations)

kernel/arch/x86_64/sched/
├── sched.cpp               (existing — boot task init; add context_switch)
├── context_switch.S        (NEW — x86_64 register save/restore + stack switch)
└── thread_cpu_context.h    (existing — x86_64 register layout)

kernel/arch/aarch64/sched/
├── sched.cpp               (existing — boot task init; add context_switch)
├── context_switch.S        (NEW — AArch64 register save/restore + stack switch)
└── thread_cpu_context.h    (existing — AArch64 register layout)
```

---

## 10. Phased Roadmap (Revised)

### Phase 1: Scheduler Core (THIS PHASE)
- Define `task` struct (wrapping `task_exec_core`)
- Task states and lifecycle
- `sched::init()`, `sched::create_kernel_task()`, `sched::enqueue()`
- Global runqueue
- `sched::yield()`, `sched::schedule()`, `sched::exit()`
- `sched::reschedule()` stub (no-op, ready for timer)
- Context switch on x86_64 and AArch64
- Idle task integration
- Boot creates idle + test tasks, they cooperatively switch
- **Deliverable:** Tasks run and voluntarily context-switch on both architectures

### Phase 2: Timer + Preemption (FUTURE)
- Timer abstraction (LAPIC on x86, Generic Timer on ARM)
- Timer ISR calls `sched::reschedule()`
- Timeslice management
- **Deliverable:** Tasks are preempted by timer; no scheduler structural changes needed

### Phase 3: Blocking + Synchronization (FUTURE)
- `sched::block()` / `sched::wake()`
- Wait queues
- Sleeping mutexes / semaphores
- **Deliverable:** Tasks can block on events and be woken

### Phase 4: SMP (FUTURE)
- Per-CPU runqueues
- Load balancing, migration, CPU affinity
- IPI-based cross-CPU wakeup
- **Deliverable:** Scheduler scales across CPUs

### Phase 5: Process Model (FUTURE)
- `process` struct (address space, file table, signal handling)
- `fork()` / `exec()` semantics (or Stellux equivalents)
- Multi-threaded process support
- **Deliverable:** Full process/thread model

---

*This document is a living brainstorm. Decisions are recorded as they are made.*
