# Stellux 3.0 Scheduler Design

> **Status:** Design discussion — API, data structures, and complete system.

---

## Part 1: Context Switch Mechanism (Settled)

`yield()` triggers a software interrupt. The existing trap entry/exit path saves and restores the complete CPU state via `trap_frame`. The scheduler just swaps the trap_frame contents:

```cpp
void sched::schedule(trap_frame* tf) {
    task* prev = current();
    task* next = pick_next();

    prev->saved_frame = *tf;        // save current task
    *tf = next->saved_frame;        // load next task

    this_cpu(current_task) = &next->exec;
    // + bookkeeping (state transitions, TSS update, etc.)
}
```

The trap exit path restores the modified trap_frame. `iretq`/`eret` resumes the next task at the correct instruction, stack, and privilege level. No custom assembly for the switch. Full details in previous revision.

---

## Part 2: Architecture-Specific `saved_state` Type

The trap_frame differs per architecture. To use it in common code without `#ifdef`:

Each architecture provides `kernel/arch/*/sched/saved_state.h`:

```cpp
// kernel/arch/x86_64/sched/saved_state.h
#ifndef STELLUX_SCHED_SAVED_STATE_H
#define STELLUX_SCHED_SAVED_STATE_H
#include "trap/trap_frame.h"
namespace sched {
    using saved_state = x86::trap_frame;
}
#endif

// kernel/arch/aarch64/sched/saved_state.h
#ifndef STELLUX_SCHED_SAVED_STATE_H
#define STELLUX_SCHED_SAVED_STATE_H
#include "trap/trap_frame.h"
namespace sched {
    using saved_state = aarch64::trap_frame;
}
#endif
```

The build system's `-Iarch/$(ARCH)` picks the right one. Common code does `#include "sched/saved_state.h"` — same pattern as `"hw/cpu.h"`.

---

## Part 3: Complete Data Structures

### 3.1 Intrusive Linked List

```cpp
// kernel/sched/list.h

namespace sched {

struct list_node {
    list_node* next;
    list_node* prev;
};

void list_init(list_node* head);
bool list_empty(const list_node* head);
void list_add_tail(list_node* node, list_node* head);
void list_remove(list_node* node);
list_node* list_pop_front(list_node* head);

template<typename T>
T* list_entry(list_node* node, list_node T::*member);

} // namespace sched
```

Used by runqueues, process task lists, future wait queues.

### 3.2 Process

```cpp
// kernel/sched/task.h

namespace sched {

struct process {
    uint32_t        pid;
    uint32_t        thread_count;
    list_node       task_list;      // head: all tasks in this process
    sync::spinlock  lock;           // protects task_list and thread_count

    // Future (added as subsystems arrive):
    // address_space*  as;
    // file_table*     files;
    // signal_state*   signals;
};

} // namespace sched
```

Present from day one. First task in a process has `tid == pid`. Future fields are added here — not in `task`.

### 3.3 Task

```cpp
// kernel/sched/task.h

namespace sched {

constexpr size_t TASK_NAME_MAX = 32;

enum class task_state : uint8_t {
    created,    // allocated, not on any runqueue
    ready,      // on a runqueue, eligible to be picked
    running,    // executing on a CPU
    blocked,    // waiting for an event (future)
    dead        // exited, pending cleanup
};

struct task {
    // === Execution core (must be first — assembly accesses at fixed offsets) ===
    task_exec_core exec;

    // === Saved CPU state (full trap_frame, saved when not running) ===
    saved_state saved_frame;

    // === Identity ===
    uint32_t    tid;            // unique task ID (monotonic)
    process*    proc;           // owning process

    // === Scheduler state ===
    task_state  state;
    uint8_t     priority;       // reserved for future policies

    // === Debug ===
    char name[TASK_NAME_MAX];

    // === Entry point (for new tasks) ===
    void (*entry_fn)(void*);
    void* entry_arg;

    // === Linkage ===
    list_node   runq_node;      // links into runqueue (managed by policy)
    list_node   proc_node;      // links into process->task_list
    // Future: list_node wait_node;
};

} // namespace sched
```

**Key decisions:**
- `exec` is first member: `(task_exec_core*)task_ptr == &task_ptr->exec`. Assembly compatibility preserved.
- `saved_frame` stores the complete CPU state. Copied to/from the trap_frame on the stack during context switch.
- `entry_fn` and `entry_arg` stored in the struct (needed for setup and debugging).
- Per-task system stack: `exec.system_stack_top` already exists. Allocated via `vmm::alloc_stack()` during task creation.

### 3.4 Per-CPU Runqueue

```cpp
// kernel/sched/runqueue.h

namespace sched {

template<typename Policy>
struct runqueue {
    sync::spinlock          lock;
    uint32_t                nr_running;
    task*                   idle_task;
    typename Policy::rq_data data;
};

} // namespace sched
```

One per CPU. Policy-specific data is type-safe via the template.

---

## Part 4: Scheduling Policy (Using Templates)

### 4.1 The Policy Contract

A scheduling policy is a struct with a nested `rq_data` type and static member functions:

```cpp
struct some_policy {
    static constexpr const char* name = "...";

    // Per-runqueue data (the policy's internal data structure).
    struct rq_data { /* ... */ };

    // Initialize rq_data (called once per CPU).
    static void init(rq_data& d);

    // Add a READY task to the runqueue.
    static void enqueue(rq_data& d, task* t);

    // Remove a task from the runqueue (blocked, dead, migrated).
    static void dequeue(rq_data& d, task* t);

    // Select the best task and remove it from the runqueue. nullptr if empty.
    static task* pick_next(rq_data& d);

    // Timer tick callback (future). Policy updates accounting here.
    static void tick(rq_data& d, task* t);
};
```

### 4.2 Round-Robin (Initial Policy)

```cpp
// kernel/sched/round_robin.h

namespace sched {

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

} // namespace sched
```

Enqueue at tail, pick from head. O(1) everything. 

### 4.3 Swapping to a Future Policy

In `sched.cpp`, change one line:

```cpp
using policy_t = round_robin;     // swap to: cfs, mlfq, priority, etc.
```

Everything else stays the same. The public API doesn't change.

---

## Part 5: Scheduler API — `kernel/sched/sched.h`

This is the interface the rest of the kernel uses. Let's go through every function, what it does, who calls it, and when.

```cpp
#ifndef STELLUX_SCHED_SCHED_H
#define STELLUX_SCHED_SCHED_H

#include "common/types.h"

namespace sched {

// Forward declarations
struct task;
struct process;

// ============================================================================
// Error codes
// ============================================================================

constexpr int32_t OK              = 0;
constexpr int32_t ERR_NO_MEMORY   = -1;
constexpr int32_t ERR_INVALID_ARG = -2;

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize the scheduler on the current CPU.
 *
 * Sets up this CPU's per-CPU runqueue, initializes the scheduling policy,
 * and converts the boot task into the idle task for this CPU.
 *
 * On the BSP: call once after mm::init() and sched::init_boot_task().
 * On APs (future): each AP calls this during its bringup sequence.
 *
 * @return OK on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

// ============================================================================
// Task Creation
// ============================================================================

/**
 * @brief Create a new kernel task in a new single-threaded process.
 *
 * Allocates:
 *   - A process struct (heap)
 *   - A task struct (heap)
 *   - A task stack (vmm::alloc_stack, 16KB + guard page)
 *   - A system stack (vmm::alloc_stack, 16KB + guard page)
 *
 * Fills in the task's saved_frame so the first context switch to it
 * begins executing entry(arg) at the desired privilege level.
 *
 * The task starts in task_state::created. It is NOT on any runqueue.
 * Call enqueue() to make it schedulable.
 *
 * @param entry  Function the task will execute.
 * @param arg    Argument passed to entry (via RDI on x86_64, x0 on AArch64).
 * @param name   Debug name (truncated to TASK_NAME_MAX - 1).
 * @return Task pointer, or nullptr on allocation failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE task* create_kernel_task(
    void (*entry)(void*),
    void* arg,
    const char* name
);

// Future:
// task* create_thread(process* proc, void (*entry)(void*), void* arg, const char* name);
// task* create_user_task(...);

// ============================================================================
// Enqueueing
// ============================================================================

/**
 * @brief Make a task schedulable.
 *
 * Moves the task from task_state::created to task_state::ready and adds it
 * to the current CPU's runqueue via the active scheduling policy.
 *
 * @param t Task in task_state::created.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enqueue(task* t);

// ============================================================================
// Yielding
// ============================================================================

/**
 * @brief Voluntarily give up the CPU.
 *
 * Triggers a software interrupt:
 *   - x86_64: int SCHED_VECTOR
 *   - AArch64: svc SYS_YIELD
 *
 * The trap entry saves the caller's complete CPU state. The trap handler
 * calls schedule(), which picks the next task and swaps the trap_frame.
 * The trap exit restores the new task's state. iretq/eret resumes it.
 *
 * When the caller is eventually scheduled again, yield() "returns" —
 * from the caller's perspective, it looks like yield() was a normal
 * function call that took a long time.
 *
 * Can be called from any privilege level (Ring 0 or Ring 3, EL0 or EL1).
 * The interrupt-routed context switch preserves the caller's privilege level.
 */
void yield();

// ============================================================================
// Scheduling (internal — called from trap handler)
// ============================================================================

/**
 * @brief The core scheduling function.
 *
 * Called by the trap handler when it recognizes a scheduling event
 * (yield interrupt, or future timer interrupt via reschedule()).
 *
 * What it does:
 *   1. If the current task is still running (voluntary yield), sets it to
 *      ready and enqueues it via the policy.
 *   2. Asks the policy for the next task to run (pick_next).
 *   3. If no tasks are ready, selects the idle task.
 *   4. Saves the current task's state: prev->saved_frame = *tf
 *   5. Loads the next task's state:    *tf = next->saved_frame
 *   6. Updates per-CPU current_task.
 *   7. Updates TSS.RSP0 (x86) / system stack (AArch64) for the next task.
 *
 * When this function returns, the trap_frame on the stack contains the
 * next task's register values. The trap exit path restores them.
 *
 * NOT called directly by tasks. Only by the trap handler.
 *
 * @param tf Pointer to the trap_frame on the system stack.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void schedule(saved_state* tf);

/**
 * @brief Preemption entry point for the timer ISR.
 *
 * Called by the timer interrupt handler. Gives the policy a chance to
 * update accounting (via tick()), then calls schedule() if preemption
 * is warranted.
 *
 * Currently a stub — the timer subsystem does not exist yet. When it
 * does, the timer ISR calls this function, and preemption works without
 * any changes to the scheduler core.
 *
 * @param tf Pointer to the trap_frame on the system stack.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void reschedule(saved_state* tf);

// ============================================================================
// Termination
// ============================================================================

/**
 * @brief Terminate the current task.
 *
 * Sets the task's state to task_state::dead, decrements the owning process's
 * thread count, and triggers a yield to switch to the next task.
 *
 * Does not return. The dead task's resources are not immediately freed —
 * a future reaper mechanism will clean them up.
 *
 * When a task's entry function returns, the exit stub calls this
 * automatically.
 */
[[noreturn]] void exit();

// ============================================================================
// Blocking (future — state machine supports it now)
// ============================================================================

// /**
//  * @brief Block the current task.
//  *
//  * Sets the task's state to task_state::blocked and triggers a yield.
//  * The task will not be enqueued on the runqueue. It stays off all
//  * queues until wake() is called on it.
//  *
//  * Typically called by wait queue implementations, not directly by tasks.
//  */
// void block();

// /**
//  * @brief Wake a blocked task.
//  *
//  * Moves the task from task_state::blocked to task_state::ready and
//  * enqueues it on a runqueue. The task becomes eligible for scheduling.
//  *
//  * Can be called from interrupt context (e.g., an I/O completion handler).
//  *
//  * @param t Task in task_state::blocked.
//  */
// void wake(task* t);

// ============================================================================
// Queries
// ============================================================================

/**
 * @brief Get the currently running task on this CPU.
 *
 * Recovers the full task* from the per-CPU current_task (task_exec_core*)
 * using the fact that exec is the first member of task.
 *
 * @return The current task.
 */
task* current();

} // namespace sched

#endif // STELLUX_SCHED_SCHED_H
```

### 5.1 API Discussion

**Who calls what:**

| Function | Called by | When |
|----------|----------|------|
| `init()` | `stlx_init()` (boot) | Once per CPU, after `mm::init()` |
| `create_kernel_task()` | Kernel subsystems | To spawn new kernel tasks |
| `enqueue()` | Kernel subsystems | After creating a task, to make it schedulable |
| `yield()` | Running tasks | Voluntary CPU release |
| `schedule()` | Trap handler only | On yield interrupt or timer interrupt |
| `reschedule()` | Timer ISR only | On each timer tick |
| `exit()` | Task exit stub, or tasks directly | When a task is done |
| `current()` | Anywhere | To get the running task |

**Separation of concerns:**

- `yield()` is the ONLY function a running task calls to give up the CPU. It's just a software interrupt — one instruction.
- `schedule()` is the ONLY function that actually does the context switch. It's called exclusively from the trap handler. Tasks never call it.
- `enqueue()` is how tasks enter the runqueue. It's called after creation, and internally by `schedule()` (to re-enqueue the yielding task).
- The policy is completely hidden behind `schedule()` and `enqueue()`. External code doesn't interact with it at all.

**The two-step create + enqueue pattern:**

```cpp
task* t = sched::create_kernel_task(my_func, my_arg, "worker");
// configure t if needed (affinity, priority, etc.)
sched::enqueue(t);
```

Separating creation from enqueueing lets the caller configure the task before it becomes visible to the scheduler. This avoids races where the task starts running before it's fully set up.

**`schedule()` takes `saved_state* tf`:**

This is necessary because `schedule()` modifies the trap_frame directly on the stack. The trap handler passes its `tf` argument straight through. The function signature makes the data flow explicit — `schedule()` reads and writes `*tf`.

This also means `schedule()` is NOT a general-purpose function that any code can call. It can ONLY be called from a context where there's a valid trap_frame on the stack (i.e., inside a trap handler). This constraint is intentional and self-documenting via the parameter type.

---

## Part 6: Scheduler Core Implementation

```cpp
// kernel/sched/sched.cpp

#include "sched/sched.h"
#include "sched/task.h"
#include "sched/runqueue.h"
#include "sched/round_robin.h"      // ← policy selection (change to swap)
#include "sched/list.h"
#include "sync/spinlock.h"
#include "percpu/percpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "common/logging.h"

namespace sched {

// ============================================================================
// Policy selection
// ============================================================================

using policy_t = round_robin;

// ============================================================================
// Per-CPU state
// ============================================================================

DEFINE_PER_CPU(runqueue<policy_t>, cpu_rq);

// ============================================================================
// ID allocation
// ============================================================================

static uint32_t next_tid = 1;
static uint32_t next_pid = 1;
static sync::spinlock id_lock = sync::SPINLOCK_INIT;

static uint32_t alloc_tid() {
    sync::irq_lock_guard guard(id_lock);
    return next_tid++;
}

static uint32_t alloc_pid() {
    sync::irq_lock_guard guard(id_lock);
    return next_pid++;
}

// ============================================================================
// Exit stub — when entry() returns, call sched::exit()
// ============================================================================

extern "C" [[noreturn]] void sched_task_exit_stub() {
    sched::exit();
}

// ============================================================================
// Arch-specific helpers (implemented in kernel/arch/*/sched/sched.cpp)
// ============================================================================

// Fill in t->saved_frame so the first context switch to t starts entry(arg).
extern void arch_setup_task_frame(task* t);

// Update the hardware so the next interrupt from this task uses its system stack.
// x86_64: writes TSS.RSP0. AArch64: stores for SP_EL1 switch.
extern void arch_switch_system_stack(task* t);

// ============================================================================
// Initialization
// ============================================================================

__PRIVILEGED_CODE int32_t init() {
    auto& rq = this_cpu(cpu_rq);
    rq.lock = sync::SPINLOCK_INIT;
    rq.nr_running = 0;
    policy_t::init(rq.data);

    // Convert the boot task (already created by init_boot_task)
    // into this CPU's idle task.
    // The boot task is currently a static task_exec_core. We need a full task.
    // Allocate a task struct and copy the boot exec core into it.
    task* idle = heap::kalloc_new<task>();
    if (!idle) return ERR_NO_MEMORY;

    idle->exec = *this_cpu(current_task);   // copy boot task's exec fields
    idle->tid = 0;                          // idle tasks get TID 0
    idle->proc = nullptr;                   // idle task has no process
    idle->state = task_state::running;
    idle->priority = 255;                   // lowest possible
    idle->saved_frame = {};
    idle->entry_fn = nullptr;
    idle->entry_arg = nullptr;
    const char* idle_name = "idle";
    for (size_t i = 0; idle_name[i]; i++) idle->name[i] = idle_name[i];
    idle->name[4] = '\0';

    this_cpu(current_task) = &idle->exec;
    rq.idle_task = idle;

    log::info("sched: initialized on CPU %u (policy: %s)", 0, policy_t::name);
    return OK;
}

// ============================================================================
// Task creation
// ============================================================================

static void copy_name(char* dst, const char* src) {
    size_t i = 0;
    while (src[i] && i < TASK_NAME_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

__PRIVILEGED_CODE task* create_kernel_task(
    void (*entry)(void*), void* arg, const char* name
) {
    if (!entry || !name) return nullptr;

    // --- Allocate process ---
    process* proc = heap::kalloc_new<process>();
    if (!proc) return nullptr;

    proc->pid = alloc_pid();
    proc->thread_count = 1;
    list_init(&proc->task_list);
    proc->lock = sync::SPINLOCK_INIT;

    // --- Allocate task ---
    task* t = heap::kalloc_new<task>();
    if (!t) {
        heap::kfree_delete(proc);
        return nullptr;
    }

    t->tid = alloc_tid();
    t->proc = proc;
    t->state = task_state::created;
    t->priority = 0;
    t->entry_fn = entry;
    t->entry_arg = arg;
    copy_name(t->name, name);

    // --- Allocate task stack (16KB usable + 1 guard page) ---
    uintptr_t ts_base, ts_top;
    if (vmm::alloc_stack(4, 1, kva::tag::privileged_stack, ts_base, ts_top) != vmm::OK) {
        heap::kfree_delete(t);
        heap::kfree_delete(proc);
        return nullptr;
    }

    // Place the exit stub's address at the top of the stack as a return address.
    // When entry() does `ret`, it returns to sched_task_exit_stub → sched::exit().
    ts_top -= sizeof(uintptr_t);
    *reinterpret_cast<uintptr_t*>(ts_top) = reinterpret_cast<uintptr_t>(sched_task_exit_stub);

    t->exec.task_stack_top = ts_top;

    // --- Allocate system stack (16KB usable + 1 guard page) ---
    uintptr_t ss_base, ss_top;
    if (vmm::alloc_stack(4, 1, kva::tag::privileged_stack, ss_base, ss_top) != vmm::OK) {
        vmm::free(ts_base);
        heap::kfree_delete(t);
        heap::kfree_delete(proc);
        return nullptr;
    }
    t->exec.system_stack_top = ss_top;

    // --- Set exec flags ---
    t->exec.flags = TASK_FLAG_KERNEL | TASK_FLAG_CAN_ELEVATE
                  | TASK_FLAG_PREEMPTIBLE | TASK_FLAG_ELEVATED;
    t->exec.cpu = 0;

    // --- Build the initial saved_frame ---
    // Arch-specific: fills in t->saved_frame so the first schedule()
    // that picks this task causes the exit path to start entry(arg).
    arch_setup_task_frame(t);

    // --- Link into process ---
    list_add_tail(&t->proc_node, &proc->task_list);

    log::debug("sched: created task '%s' tid=%u pid=%u", t->name, t->tid, proc->pid);
    return t;
}

// ============================================================================
// Enqueueing
// ============================================================================

__PRIVILEGED_CODE void enqueue(task* t) {
    auto& rq = this_cpu(cpu_rq);
    auto irq = sync::spin_lock_irqsave(rq.lock);

    t->state = task_state::ready;
    policy_t::enqueue(rq.data, t);
    rq.nr_running++;

    sync::spin_unlock_irqrestore(rq.lock, irq);
}

// ============================================================================
// Core scheduling
// ============================================================================

__PRIVILEGED_CODE void schedule(saved_state* tf) {
    auto& rq = this_cpu(cpu_rq);
    auto irq = sync::spin_lock_irqsave(rq.lock);

    task* prev = current();
    
    // Re-enqueue if prev yielded voluntarily (still "running").
    // If prev is blocked or dead, don't re-enqueue.
    if (prev->state == task_state::running) {
        prev->state = task_state::ready;
        policy_t::enqueue(rq.data, prev);
        rq.nr_running++;
    }

    // Ask the policy.
    task* next = policy_t::pick_next(rq.data);
    if (next) {
        rq.nr_running--;
    } else {
        next = rq.idle_task;
    }

    sync::spin_unlock_irqrestore(rq.lock, irq);

    if (prev == next) {
        prev->state = task_state::running;
        return;
    }

    // === THE CONTEXT SWITCH ===
    prev->saved_frame = *tf;        // save outgoing task
    *tf = next->saved_frame;        // load incoming task

    next->state = task_state::running;
    next->exec.cpu = 0; // TODO: this_cpu_id() for SMP
    this_cpu(current_task) = &next->exec;

    // Update hardware so next interrupt targets the new task's system stack.
    arch_switch_system_stack(next);
}

__PRIVILEGED_CODE void reschedule(saved_state* tf) {
    task* t = current();
    if (!(t->exec.flags & TASK_FLAG_PREEMPTIBLE))
        return;

    auto& rq = this_cpu(cpu_rq);
    policy_t::tick(rq.data, t);

    // Future: check needs_resched flag set by tick()
    // For now, stub — no timer, no preemption.
    (void)tf;
}

// ============================================================================
// Termination
// ============================================================================

[[noreturn]] void exit() {
    task* t = current();
    t->state = task_state::dead;

    // Decrement process thread count
    if (t->proc) {
        sync::irq_lock_guard guard(t->proc->lock);
        t->proc->thread_count--;
    }

    log::debug("sched: task '%s' tid=%u exiting", t->name, t->tid);

    // Yield — schedule() won't re-enqueue us because state is dead.
    yield();

    __builtin_unreachable();
}

// ============================================================================
// Queries
// ============================================================================

task* current() {
    // task_exec_core is the first member of task.
    return reinterpret_cast<task*>(this_cpu(current_task));
}

} // namespace sched
```

### 6.1 `arch_setup_task_frame` — x86_64

```cpp
// kernel/arch/x86_64/sched/sched.cpp (additions)

void sched::arch_setup_task_frame(sched::task* t) {
    t->saved_frame = {};    // zero everything

    t->saved_frame.rip    = reinterpret_cast<uint64_t>(t->entry_fn);
    t->saved_frame.rdi    = reinterpret_cast<uint64_t>(t->entry_arg);
    t->saved_frame.cs     = x86::KERNEL_CS;
    t->saved_frame.ss     = x86::KERNEL_DS;
    t->saved_frame.rflags = x86::RFLAGS_IF;   // interrupts enabled
    t->saved_frame.rsp    = t->exec.task_stack_top;
}
```

### 6.2 `arch_setup_task_frame` — AArch64

```cpp
// kernel/arch/aarch64/sched/sched.cpp (additions)

void sched::arch_setup_task_frame(sched::task* t) {
    t->saved_frame = {};    // zero everything

    t->saved_frame.elr    = reinterpret_cast<uint64_t>(t->entry_fn);
    t->saved_frame.x[0]   = reinterpret_cast<uint64_t>(t->entry_arg);
    t->saved_frame.spsr   = aarch64::SPSR_EL1T;  // EL1 with SP_EL0 (elevated)
    t->saved_frame.sp     = t->exec.task_stack_top;
}
```

### 6.3 `arch_switch_system_stack` — x86_64

```cpp
void sched::arch_switch_system_stack(sched::task* t) {
    // Update TSS.RSP0 so the next Ring 3 → Ring 0 interrupt
    // uses this task's system stack.
    x86::gdt::set_kernel_stack(t->exec.system_stack_top);
}
```

### 6.4 `arch_switch_system_stack` — AArch64

On AArch64, SP_EL1 is the system stack and we're currently on it during the handler. We can't change it while we're using it. For cooperative scheduling with elevated kernel tasks, all tasks share the BSP's SP_EL1. This works because:

- The trap_frame is always consumed (restored + popped) before the next yield
- The system stack is clean after each eret
- No nesting (cooperative scheduling, no nested preemption)

When user-mode tasks and preemption arrive, we'll add an SP_EL1 switch to the trap exit assembly (using x16 as a scratch register after GPR restore, since x16 is an intra-procedure-call scratch register that tasks don't rely on).

```cpp
void sched::arch_switch_system_stack(sched::task* t) {
    // For now: no-op. All tasks share the BSP's SP_EL1.
    // Future: store next system_stack_top for the exit path to load.
    (void)t;
}
```

---

## Part 7: Trap Handler Integration

### 7.1 x86_64

Reserve a vector (e.g., `SCHED_VECTOR = 0x81`):

```cpp
// kernel/arch/x86_64/trap/trap.cpp

extern "C" __PRIVILEGED_CODE void stlx_x86_64_trap_handler(x86::trap_frame* tf) {
    sched::task_exec_core* task_exec = this_cpu(current_task);
    task_exec->flags |= sched::TASK_FLAG_IN_IRQ;

    if (tf->vector == x86::SCHED_VECTOR) {
        sched::schedule(tf);
        // current_task may have changed — reload for the flag clear below
        task_exec = this_cpu(current_task);
        task_exec->flags &= ~sched::TASK_FLAG_IN_IRQ;
        return;
    }

    // ... existing fatal/exception handling ...
    task_exec->flags &= ~sched::TASK_FLAG_IN_IRQ;
}
```

`yield()` on x86_64:
```cpp
void sched::yield() {
    asm volatile("int %0" :: "i"(x86::SCHED_VECTOR) : "memory");
}
```

### 7.2 AArch64

Use a dedicated SVC number (e.g., `SYS_YIELD = 998`):

```cpp
// kernel/arch/aarch64/trap/trap.cpp

extern "C" __PRIVILEGED_CODE
void stlx_aarch64_el0_sync_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    const uint64_t esr = tf->esr;
    const uint8_t ec = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);

    if (ec == aarch64::EC_SVC_A64) {
        uint16_t svc_num = static_cast<uint16_t>(esr & 0xFFFF);
        if (svc_num == syscall::SYS_YIELD) {
            sched::schedule(tf);
            return;
        }
        stlx_aarch64_syscall_dispatch(tf);
        return;
    }
    trap_fatal("el0 sync", tf);
}

// Same dispatch added to stlx_aarch64_el1_sync_handler for elevated yields.
```

`yield()` on AArch64:
```cpp
void sched::yield() {
    asm volatile("svc %0" :: "i"(syscall::SYS_YIELD) : "memory");
}
```

---

## Part 8: Boot Sequence

```cpp
// kernel/boot/boot.cpp

extern "C" __PRIVILEGED_CODE void stlx_init() {
    boot_services::init();
    serial::init();
    arch::early_init();        // sets up GDT/IDT/VBAR, calls sched::init_boot_task()
    mm::init();

    // NEW: initialize the scheduler
    sched::init();             // converts boot task to idle, sets up runqueue

    // NEW: create test tasks
    sched::task* t1 = sched::create_kernel_task(task_a_func, nullptr, "task-a");
    sched::task* t2 = sched::create_kernel_task(task_b_func, nullptr, "task-b");
    sched::enqueue(t1);
    sched::enqueue(t2);

    // Start scheduling — yield from the idle task
    sched::yield();

    // Idle loop (when all tasks are done, we end up back here)
    while (true) {
        cpu::halt();
    }
}
```

---

## Part 9: File Layout (Complete)

```
kernel/sched/
├── task_exec_core.h            (existing, unchanged)
├── thread_cpu_context.h        (existing, unchanged)
├── list.h                      (NEW — intrusive linked list)
├── task.h                      (NEW — task, process, task_state)
├── runqueue.h                  (NEW — runqueue<Policy> template)
├── round_robin.h               (NEW — round-robin policy)
├── sched.h                     (NEW — public API)
└── sched.cpp                   (NEW — scheduler core)

kernel/arch/x86_64/sched/
├── sched.cpp                   (existing — add arch_setup_task_frame, arch_switch_system_stack)
├── saved_state.h               (NEW — using saved_state = x86::trap_frame)
└── thread_cpu_context.h        (existing)

kernel/arch/aarch64/sched/
├── sched.cpp                   (existing — add arch_setup_task_frame, arch_switch_system_stack)
├── saved_state.h               (NEW — using saved_state = aarch64::trap_frame)
└── thread_cpu_context.h        (existing)
```

No new assembly files. Context switch is in C++. Trap entry/exit is unchanged.

---

## Part 10: Roadmap

### Phase 1: Scheduler Core (THIS PHASE)
- Intrusive list utility
- Process and task structs
- Per-CPU runqueue with round-robin policy
- `init()`, `create_kernel_task()`, `enqueue()`
- `yield()` via software interrupt
- `schedule()` — trap_frame swap context switch
- `exit()` via exit stub
- `reschedule()` stub
- Arch: `saved_state.h`, `arch_setup_task_frame`, `arch_switch_system_stack`
- Trap handler dispatch for yield vector/SVC
- Boot creates idle + test tasks, cooperative round-robin
- **Deliverable:** Tasks run and cooperatively switch on both architectures

### Phase 2: Timer + Preemption
- Timer abstraction (LAPIC / Generic Timer)
- Timer ISR calls `reschedule(tf)`
- Policy `tick()` updates accounting + sets needs_resched
- `reschedule()` calls `schedule()` when needed
- **Deliverable:** Preemption works; zero scheduler core changes

### Phase 3: Blocking + Synchronization
- `block()` / `wake()`
- Wait queues (reusing `list_node`)
- Sleeping mutexes
- **Deliverable:** Tasks block on events

### Phase 4: SMP
- Each AP calls `sched::init()` → per-CPU runqueue + idle task
- `arch_switch_system_stack` fully implemented for AArch64
- Load balancing between per-CPU runqueues
- CPU affinity, migration
- IPI-based cross-CPU wakeup
- **Deliverable:** Multi-core scheduling

### Phase 5: Process Model + Userspace
- Address spaces in `process`
- `create_user_task()` / `create_thread()`
- Ring 3 / EL0 tasks with separate task stacks
- **Deliverable:** Full process/thread model

### Phase 6: Policy Evolution
- Swap round-robin for CFS, MLFQ, or priority-based
- Same core, different `using policy_t = ...;`
- **Deliverable:** Production scheduling

---

*This design is intentionally minimal. The existing trap infrastructure does the heavy lifting. The scheduler core is ~150 lines of C++. The policy is ~20 lines. No custom assembly for the context switch.*
