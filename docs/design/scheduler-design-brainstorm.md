# Stellux 3.0 Scheduler Design Brainstorm

> **Status:** Early design exploration — no code yet.
> **Goal:** Define the semantics, structure, and key design decisions for the Stellux 3.0 scheduler subsystem before writing any implementation.

---

## 1. What We're Building On

The kernel already has:

- **`task_exec_core`** — minimal task descriptor with flags, CPU affinity, dual stack pointers (task + system), and arch-specific CPU context (`thread_cpu_context`)
- **Task flags** — `ELEVATED`, `KERNEL`, `CAN_ELEVATE`, `IDLE`, `RUNNING`, `IN_SYSCALL`, `IN_IRQ`, `PREEMPTIBLE`, `FPU_USED`
- **Per-CPU infrastructure** — `DEFINE_PER_CPU`, `this_cpu()`, per-CPU offsets, cacheline-aligned variants
- **`current_task`** — per-CPU pointer to the running task
- **Boot task** — initialized during `arch::early_init()`, marked as elevated + kernel + idle + running
- **Dynamic privilege** — `dynpriv::elevate()`, `dynpriv::lower()`, `RUN_ELEVATED()` macro
- **Synchronization** — ticket spinlocks with IRQ-save variants and RAII guards
- **Memory management** — PMM (buddy allocator), KVA, VMM with stack allocation (`alloc_stack` with guard pages), privileged + unprivileged heaps
- **Dual architecture** — x86_64 and AArch64 are first-class; per-arch context save/restore structures are defined

---

## 2. Fundamental Design Questions

### 2.1 Task Model: What Is a "Task" in Stellux 3.0?

The existing `task_exec_core` is deliberately minimal — it captures the *execution core* of a task. But a full scheduler needs to answer:

**Q1: Is there a process/thread distinction, or is everything a single-level "task"?**

Options:
- **(a) Flat task model** — Every schedulable entity is a `task`. No process/thread hierarchy. Address space sharing is explicit. Similar to early Linux "everything is a `task_struct`" or Plan 9.
- **(b) Process + thread model** — A `process` owns an address space and resources; `threads` within it share the address space. Classic POSIX-like model.
- **(c) Task groups** — Tasks can be grouped for resource sharing (address space, file descriptors, etc.) without a strict hierarchy. More flexible than (b).

*Design pressure from dynamic privilege:* A task that can elevate/lower dynamically needs careful isolation from other tasks sharing its address space. If Task A elevates and accesses privileged memory, Task B in the same address space must not be able to read that memory while lowered. This might push toward per-task privilege tracking even within shared address spaces.

**Q2: Does `task_exec_core` remain the schedulable unit, or does it get wrapped in a larger `task` struct?**

The current structure is clean but lacks scheduling metadata (priority, timeslice, state, wait channels, etc.). Two approaches:
- **(a) Extend `task_exec_core`** — Add scheduling fields directly. Simple, but mixes concerns.
- **(b) Composition** — A new `task` struct *contains* a `task_exec_core` plus scheduling state, identity, etc. The scheduler operates on `task`, context switching operates on `task_exec_core`. Clean separation.

Recommendation: **(b)** — `task_exec_core` is the arch/execution boundary; scheduling policy belongs in a wrapper.

### 2.2 Scheduling Policy: What Algorithm?

**Q3: What scheduling class or algorithm should Stellux 3.0 use?**

Options to consider:

- **(a) Simple round-robin** — Equal timeslices, FIFO within priority levels. Good starting point. Easy to reason about.
- **(b) Multi-level feedback queue (MLFQ)** — Tasks move between priority queues based on behavior. Classic, well-understood. Used by most production kernels.
- **(c) Completely Fair Scheduler (CFS) style** — Virtual runtime tracking, red-black tree, proportional fairness. Linux's approach. More complex but well-proven.
- **(d) Fixed-priority preemptive** — Real-time style. Tasks have static priorities. Highest priority runnable task always runs.
- **(e) Pluggable scheduler classes** — Like Linux's scheduling classes (CFS, RT, deadline). Different policies coexist. Most flexible but most complex.

*Design pressure from "robustness over features":* Start simple (round-robin or simple priority-based), get it correct on both architectures, then evolve. A pluggable framework is appealing but might be premature abstraction.

*Design pressure from dynamic privilege:* Does privilege level influence scheduling priority? If an elevated task is doing time-critical kernel work, should it get priority? Or should privilege and scheduling be completely orthogonal?

### 2.3 Dynamic Privilege × Scheduling Interaction

This is the most unique and interesting design space for Stellux.

**Q4: Can an elevated task be preempted?**

Options:
- **(a) Never preempt elevated tasks** — Elevation implies a critical section. Simple, but a misbehaving elevated task could starve everything.
- **(b) Always preemptible, even when elevated** — Most flexible, but context switching while elevated requires saving/restoring privilege state correctly.
- **(c) Configurable per-task** — `TASK_FLAG_PREEMPTIBLE` already exists. Elevation could clear it by default, but tasks could opt in to being preemptible while elevated.
- **(d) Time-bounded elevation** — Elevated tasks get a grace period before forced preemption/lowering. Safety net against misbehavior.

*Note:* The existing `dynpriv::lower()` implementation on both architectures carefully disables interrupts before clearing the elevated flag and transitioning. This suggests the current design assumes elevation is short-lived and should not be interrupted mid-transition. But what about the *body* of elevated code?

**Q5: What happens when the scheduler preempts a task that is currently elevated?**

If we allow preemption of elevated tasks:
- The saved context must record the privilege level (`TASK_FLAG_ELEVATED`)
- When the task is resumed, it must resume at the *same privilege level*
- On x86_64: this means resuming into Ring 0 (possibly via `iret` to Ring 0 rather than `sysret` to Ring 3)
- On AArch64: this means `eret` back to EL1 rather than EL0
- The dual-stack model (`task_stack_top` vs `system_stack_top`) needs to handle this correctly

**Q6: Should there be a scheduling penalty or boost for elevated tasks?**

- **(a) No interaction** — Privilege and scheduling are orthogonal. Cleanest separation.
- **(b) Priority boost while elevated** — Elevated tasks are doing kernel-critical work, finish it faster.
- **(c) Time accounting** — Track time spent elevated separately. Count it toward the task's quantum or don't.

### 2.4 Preemption Model

**Q7: What preemption model should Stellux 3.0 use?**

- **(a) Non-preemptive (cooperative)** — Tasks yield voluntarily. Simple, but bad for responsiveness.
- **(b) Preemptive in userspace only** — Kernel code runs to completion (or explicit yield). No preemption during syscalls or while elevated. Simpler kernel synchronization.
- **(c) Fully preemptive** — Any task can be preempted at any time (except in explicitly non-preemptible sections). Best responsiveness, most complex synchronization.
- **(d) Voluntary preemption** — Like (b) but with explicit preemption points in long kernel paths. Middle ground.

*Design pressure from dynamic privilege:* With dynamic privilege, the line between "userspace" and "kernel" is blurred. A task might be running user code, elevate to do some privileged operation, then lower back. Preemption model (b) would mean the task becomes non-preemptible whenever it elevates — is that desirable?

### 2.5 SMP and Per-CPU Scheduling

**Q8: Per-CPU runqueues or global runqueue?**

- **(a) Global runqueue** — Single queue, any CPU picks from it. Simple, but lock contention at scale.
- **(b) Per-CPU runqueues** — Each CPU has its own queue. Load balancing needed. Linux-style. Better scalability.
- **(c) Start global, evolve to per-CPU** — Get it working first, optimize later.

*Design pressure:* Per-CPU infrastructure is already solid. `current_task` is per-CPU. The architecture supports per-CPU runqueues naturally.

**Q9: CPU affinity and migration?**

- Should tasks be able to specify CPU affinity?
- When/how does migration between CPUs happen?
- Does elevation affect migration? (e.g., should an elevated task be pinned to its CPU?)

### 2.6 Task Lifecycle and States

**Q10: What are the task states?**

Proposed state machine:

```
CREATED  →  READY  →  RUNNING  →  BLOCKED  →  READY
                  ↑                    ↓
                  └────────────────────┘
                          RUNNING → ZOMBIE → (reaped)
```

- **CREATED** — Allocated but not yet schedulable
- **READY** — Runnable, waiting for CPU time
- **RUNNING** — Currently executing on a CPU
- **BLOCKED** — Waiting for an event (I/O, lock, timer, IPC)
- **ZOMBIE** — Exited, waiting to be reaped/cleaned up

**Q11: How do tasks block and wake up?**

Need a wait/wake mechanism:
- Wait queues (list of tasks waiting for a condition)
- `sleep()`/`wake()` primitives
- Integration with synchronization (mutexes built on wait queues + spinlocks)
- Integration with I/O (block on read, wake on interrupt)

### 2.7 Context Switching

**Q12: What does a context switch look like?**

Architecture split:
- **Common:** Selecting the next task, updating runqueue state, accounting
- **Arch-specific:** Saving/restoring registers, switching stacks, switching address spaces (when we have them), TLB management

The existing `thread_cpu_context` on both architectures defines the register set to save. The key additions needed:
- A `switch_to(prev, next)` function (arch-specific assembly)
- FPU/SIMD state save/restore (lazy or eager — `TASK_FLAG_FPU_USED` suggests lazy)
- Stack switching (using `task_stack_top` / `system_stack_top`)

**Q13: FPU state management — lazy or eager?**

- **Lazy:** Don't save FPU state until another task tries to use FPU. Saves time when few tasks use FPU. `TASK_FLAG_FPU_USED` already exists for this.
- **Eager:** Always save/restore FPU state on context switch. Simpler, eliminates device-not-available faults. Modern CPUs make this fast (XSAVE/XRSTOR on x86, FPCR/FPSR + V0-V31 on ARM).

### 2.8 Timer and Tick Model

**Q14: Tick-based or tickless?**

- **(a) Periodic tick** — Timer fires at fixed interval (e.g., 1000 Hz). Simple. Each tick decrements the running task's timeslice.
- **(b) Tickless (NO_HZ)** — Timer fires only when needed (next event). Better power efficiency. More complex.
- **(c) Start with periodic tick, evolve to tickless** — Pragmatic approach.

*Design pressure:* Both architectures have different timer hardware (LAPIC timer on x86, Generic Timer on ARM). Need a common timer abstraction.

---

## 3. Proposed Initial Architecture

Based on Stellux's philosophy of "robustness over features" and "semantics first," here's a proposed starting architecture:

### Layer Diagram

```
┌─────────────────────────────────────────────┐
│            Scheduler Policy Layer           │
│  (algorithm, runqueues, priority, timeslice)│
│              kernel/sched/                  │
├─────────────────────────────────────────────┤
│            Task Management Layer            │
│  (task lifecycle, state, wait/wake, flags)  │
│              kernel/sched/                  │
├─────────────────────────────────────────────┤
│          Context Switch Boundary            │
│    (save/restore registers, switch stack)   │
│         kernel/arch/*/sched/                │
├─────────────────────────────────────────────┤
│          Timer / Tick Abstraction           │
│        (periodic tick, preemption)          │
│    kernel/common/ + kernel/arch/*/          │
├─────────────────────────────────────────────┤
│     Dynamic Privilege Integration           │
│  (elevation-aware context switch, flags)    │
│         kernel/dynpriv/ + sched/            │
└─────────────────────────────────────────────┘
```

### Proposed Task Structure (Conceptual)

```
task (full scheduler entity)
├── task_exec_core         (execution state — existing)
│   ├── flags              (runtime flags)
│   ├── cpu                (current CPU)
│   ├── task_stack_top     (user/lowered stack)
│   ├── system_stack_top   (kernel/elevated stack)
│   └── cpu_ctx            (arch-specific registers)
├── sched_entity           (scheduling metadata — new)
│   ├── state              (READY, RUNNING, BLOCKED, ...)
│   ├── priority           (scheduling priority)
│   ├── timeslice_remaining
│   ├── total_runtime      (accounting)
│   └── runqueue_link      (intrusive list node)
├── task_id                (unique identifier)
├── name                   (debug name)
└── wait_channel           (what this task is waiting on)
```

### Proposed Scheduling Algorithm (Initial)

**Simple priority-based round-robin:**
- Fixed number of priority levels (e.g., 32 or 64)
- Round-robin within each priority level
- Highest-priority non-empty queue is always serviced first
- Timer tick decrements timeslice; when exhausted, move to back of queue

Why this over CFS or MLFQ:
- Predictable, easy to reason about and debug
- Gets context switching and preemption correct first
- Can evolve to MLFQ/CFS once the foundation is solid
- Works identically on both architectures (no arch-specific policy)

---

## 4. Dynamic Privilege: Deep Design Considerations

### The Core Tension

Dynamic privilege creates a unique design tension: a task's hardware privilege level can change *during execution*, but scheduling typically treats tasks as having fixed properties. Key scenarios:

**Scenario A: Task elevates, timer fires**
1. Task is running at EL0/Ring 3
2. Task calls `dynpriv::elevate()` → now at EL1/Ring 0
3. Timer interrupt fires
4. Scheduler decides to preempt
5. Must save state as "elevated" — resume later at EL1/Ring 0

**Scenario B: Task elevates, blocks**
1. Task elevates
2. Task calls a blocking operation (future: I/O, mutex)
3. Task is saved as elevated + blocked
4. When woken, must resume at EL1/Ring 0
5. Task continues elevated work, then lowers

**Scenario C: Nested elevation awareness**
1. Task is lowered, calls `RUN_ELEVATED { ... }`
2. Inside the block, another subsystem also uses `RUN_ELEVATED { ... }`
3. The inner one checks `is_elevated()` and skips the transition
4. But what if the task is preempted *inside* the inner block?

### Design Principle: Privilege Is Task State, Not Scheduler State

The scheduler should treat privilege level as part of the task's saved execution state (already captured in `TASK_FLAG_ELEVATED`), not as a scheduling input. This means:
- The scheduler doesn't care whether a task is elevated or lowered
- Context switch correctly saves and restores the privilege level
- The arch-specific `switch_to()` uses the saved privilege flag to determine how to resume

### Open Questions for Dynamic Privilege × Scheduling

1. Should `RUN_ELEVATED` disable preemption? This would be the safest default but limits concurrency.
2. Should there be a `dynpriv::elevate_nonpreemptible()` variant vs `dynpriv::elevate_preemptible()`?
3. If a task has been elevated for "too long" (no lowering), should the scheduler force-lower it? Or is that a policy violation?
4. How does elevation interact with address space switching? When elevated, the task can access privileged memory regions. If we switch address spaces, those mappings must be consistent.

---

## 5. Implementation Roadmap (Proposed Phases)

### Phase 1: Minimal Cooperative Scheduler
- Wrap `task_exec_core` in a `task` struct with scheduling metadata
- Implement `sched::create_task()`, `sched::yield()`, `sched::exit()`
- Single global runqueue (simple linked list)
- Context switch implementation for both architectures
- Round-robin, no preemption
- Boot creates idle task + one or two test tasks
- **Goal:** Tasks can run and voluntarily switch between each other

### Phase 2: Preemptive Scheduling
- Timer abstraction (LAPIC on x86, Generic Timer on ARM)
- Periodic tick handler
- Timeslice-based preemption
- Priority levels
- Correct handling of preemption during elevation
- **Goal:** Tasks are preempted fairly without deadlocks or corruption

### Phase 3: Blocking and Wait Queues
- Wait queue infrastructure
- `sched::sleep()` / `sched::wake()` primitives
- Mutex built on wait queue + spinlock
- **Goal:** Tasks can block and be woken by events

### Phase 4: SMP Scheduling
- Per-CPU runqueues
- Load balancing
- CPU affinity
- Migration
- Cross-CPU wake-up (IPI)
- **Goal:** Scheduler scales across multiple CPUs

### Phase 5: Policy Evolution
- Evaluate need for MLFQ, CFS, or deadline scheduling
- Potentially introduce scheduling classes
- **Goal:** Scheduler policy matches workload requirements

---

## 6. Open Questions Summary

| # | Question | Options | Depends On |
|---|----------|---------|------------|
| Q1 | Process/thread model or flat tasks? | Flat / Process+Thread / Task groups | Address space design |
| Q2 | Extend `task_exec_core` or compose? | Extend / Compose | — |
| Q3 | Initial scheduling algorithm? | RR / MLFQ / CFS / Fixed-priority | Complexity budget |
| Q4 | Can elevated tasks be preempted? | Never / Always / Configurable / Time-bounded | Dynamic privilege semantics |
| Q5 | How to resume a preempted elevated task? | Save privilege as task state | Architecture |
| Q6 | Scheduling penalty/boost for elevation? | None / Boost / Separate accounting | Design philosophy |
| Q7 | Preemption model? | Non-preemptive / User-only / Full / Voluntary | Complexity budget |
| Q8 | Global or per-CPU runqueues? | Global / Per-CPU / Evolve | SMP timeline |
| Q9 | CPU affinity and migration? | Yes / Deferred | SMP timeline |
| Q10 | Task states? | Created/Ready/Running/Blocked/Zombie | — |
| Q11 | Wait/wake mechanism? | Wait queues | Blocking I/O timeline |
| Q12 | Context switch structure? | Common policy + arch mechanism | — |
| Q13 | FPU management? | Lazy / Eager | Performance goals |
| Q14 | Tick model? | Periodic / Tickless / Evolve | Timer abstraction |

---

*This document is a living brainstorm. Decisions will be recorded here as they are made.*
