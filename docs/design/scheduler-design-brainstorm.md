# Stellux 3.0 Scheduler Design

> **Status:** Detailed design — brainstorming component structure and C++ API.
> **Constraints:** C++20 freestanding. No exceptions, no RTTI, no stdlib. Dual-architecture (x86_64 + AArch64).

---

## 1. High-Level Decisions (Settled)

| Decision | Choice |
|----------|--------|
| Privilege × scheduling | Orthogonal — scheduler is privilege-blind |
| Context switch model | Interrupt-routed — `yield()` triggers software interrupt, `iretq`/`eret` handles return |
| Scheduler architecture | Core + pluggable policy — core is mechanism, policy is algorithm |
| Initial algorithm | Round-robin |
| Runqueue topology | Per-CPU from day one |
| Task model | Flat tasks with lightweight `process` grouping |
| `task_exec_core` | Composition — `task` wraps it |

---

## 2. Scheduling Policy: C++ Abstraction Design

### 2.1 The Question

The scheduling policy answers one question: "given a set of ready tasks, which runs next?" We want this to be swappable without changing the scheduler core. In freestanding C++20 (no RTTI, no exceptions), what's the right abstraction?

### 2.2 Options Considered

**Option A: C-style function pointer table**
```cpp
struct sched_policy_ops {
    void (*init)(void* rq_data);
    void (*enqueue)(void* rq_data, task* t);
    task* (*pick_next)(void* rq_data);
    // ...
};
```
Runtime-swappable. But: `void*` is not type-safe. Feels like C, not C++. Indirect calls add overhead. The opaque `policy_data[128]` byte array in the runqueue is fragile — wrong size breaks silently.

**Option B: Virtual base class**
```cpp
struct sched_policy {
    virtual void enqueue(task* t) = 0;
    virtual task* pick_next() = 0;
    virtual ~sched_policy() = default;
};
```
Compiler generates vtable. Works without RTTI. But: virtual dispatch is an indirect call (cache miss on cold path). Virtual destructors need care in freestanding. Feels heavy for something that's called on every schedule cycle. Also, the runqueue's policy-specific data still needs to live somewhere — inside the derived class? Then the runqueue holds a `sched_policy*`, and the allocation/ownership gets awkward.

**Option C: Policy as a type — static dispatch via templates**
```cpp
struct round_robin {
    struct rq_data { list_node head; };

    static void init(rq_data& data);
    static void enqueue(rq_data& data, task* t);
    static task* pick_next(rq_data& data);
    static void tick(rq_data& data, task* t);
};
```
Zero overhead — all calls are direct, inlineable. Type-safe — `rq_data` is a concrete type, no casts. Policy-specific data is a proper struct, not an opaque byte array. Compile-time interface enforcement — if the policy doesn't implement `pick_next()`, you get a clear compiler error. Follows the codebase's existing template patterns (like `kalloc_new<T>()`).

"Swapping" means changing one `using` declaration and rebuilding. The public API (`sched::yield()`, `sched::schedule()`, etc.) doesn't change. No runtime overhead.

### 2.3 Recommendation: Option C

This is the most C++-idiomatic approach for a freestanding kernel. It gives us:
- **Zero-cost abstraction** — the compiler sees through everything
- **Type-safe policy data** — each policy defines its own `rq_data` struct
- **Same external API** — callers never see the policy type
- **Compile-time swapping** — change `using policy_t = round_robin;` to `using policy_t = cfs;`

The policy type is an implementation detail of `sched.cpp`. The public headers (`sched.h`) expose a plain namespace API. External code calls `sched::yield()` without knowing which policy is active.

---

## 3. Component-by-Component Design

### 3.1 Intrusive Linked List — `kernel/sched/list.h`

Foundation for runqueues, process task lists, and future wait queues.

```cpp
#ifndef STELLUX_SCHED_LIST_H
#define STELLUX_SCHED_LIST_H

#include "common/types.h"

namespace sched {

struct list_node {
    list_node* next;
    list_node* prev;
};

inline void list_init(list_node* head) {
    head->next = head;
    head->prev = head;
}

inline bool list_empty(const list_node* head) {
    return head->next == head;
}

inline void list_add_tail(list_node* node, list_node* head) {
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

inline void list_remove(list_node* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = nullptr;
    node->prev = nullptr;
}

inline list_node* list_pop_front(list_node* head) {
    if (list_empty(head)) return nullptr;
    list_node* front = head->next;
    list_remove(front);
    return front;
}

// Get the struct that contains this list_node.
// Usage: list_entry(node_ptr, task, runq_node)
// Equivalent to Linux's container_of / list_entry.
template<typename T>
inline T* list_entry(list_node* node, list_node T::*member) {
    // Compute the offset of `member` within T, then subtract from node address.
    const auto offset = reinterpret_cast<uintptr_t>(
        &(static_cast<T*>(nullptr)->*member)
    );
    return reinterpret_cast<T*>(
        reinterpret_cast<uintptr_t>(node) - offset
    );
}

} // namespace sched

#endif // STELLUX_SCHED_LIST_H
```

**Design notes:**
- Circular doubly-linked list (Linux `list_head` pattern).
- `list_entry<T>()` uses a pointer-to-member to compute `container_of` without macros. Type-safe, works at compile time.
- All operations are `inline` — no .cpp file needed.
- `list_remove` nulls pointers for safety (detects double-remove bugs).

### 3.2 Task State — `kernel/sched/task.h` (partial)

```cpp
enum class task_state : uint8_t {
    created,    // Allocated, not on any runqueue
    ready,      // On a runqueue, eligible to be scheduled
    running,    // Executing on a CPU
    blocked,    // Waiting for an event (future)
    dead        // Exited, pending cleanup
};
```

Using a scoped enum (`enum class`) for type safety — can't accidentally compare with integers or mix with other enums.

### 3.3 Process — `kernel/sched/task.h` (partial)

```cpp
struct process {
    uint32_t pid;
    uint32_t thread_count;
    list_node task_list;        // Head: list of all tasks in this process
    sync::spinlock lock;        // Protects task_list and thread_count

    // Future: address_space*, file_table*, signal state, ...
};
```

**Design notes:**
- Minimal. Grows as subsystems are added.
- `task_list` is a `list_node` head. Each task in this process links through its `proc_node`.
- `lock` protects modifications to the task list and thread count. The scheduler never acquires this — it uses the runqueue lock instead. These are separate concerns.
- First task in a process has `task.tid == process.pid`.

**Allocation:** `heap::kalloc_new<process>()`. Privileged heap because process metadata is kernel-internal.

### 3.4 Task — `kernel/sched/task.h` (partial)

```cpp
constexpr size_t TASK_NAME_MAX = 32;

struct task {
    // Execution core — what the arch layer and context switch operate on.
    // Must be first member so that &task == &task.exec (simplifies asm).
    task_exec_core exec;

    // Identity
    uint32_t tid;               // Unique task ID
    process* proc;              // Owning process

    // Scheduler state
    task_state state;
    uint8_t priority;           // Reserved for future policies (unused by round-robin)

    // Debug
    char name[TASK_NAME_MAX];

    // Linkage — intrusive list nodes for different lists this task can be on.
    list_node runq_node;        // Links into the runqueue (policy manages this)
    list_node proc_node;        // Links into process->task_list

    // Future: list_node wait_node, uint64_t vruntime, ...
};
```

**Design notes:**
- **`exec` is the first member.** This means `reinterpret_cast<task_exec_core*>(task_ptr) == &task_ptr->exec`. The per-CPU `current_task` is a `task_exec_core*` (for assembly compatibility — asm code accesses flags/stack at fixed offsets from the pointer). The scheduler can recover the full `task*` from the `task_exec_core*` via `container_of` or `list_entry`-style offset math.

- **Two list nodes.** A task can be simultaneously on the runqueue (`runq_node`) and in its process's task list (`proc_node`). These are independent linked lists.

- **`priority` is reserved.** Round-robin ignores it. Future policies (priority-based, CFS with nice values) will use it. Costs one byte, no harm.

**Open question: Should `task_exec_core` remain a separate struct, or should its fields be inlined into `task`?**

Arguments for keeping it separate:
- Assembly code accesses `task_exec_core` fields at fixed offsets. If `task_exec_core` is the first member of `task`, these offsets are the same whether asm sees `task*` or `task_exec_core*`.
- The existing `static_assert` checks in `task_exec_core.h` ensure asm offsets stay correct.
- Separation of concerns: `task_exec_core` is the arch/execution boundary; `task` is the scheduler entity.

Arguments for inlining:
- One fewer level of indirection in C++ code (e.g., `t->flags` instead of `t->exec.flags`).
- Simpler struct layout.

**Recommendation: Keep it separate.** The indirection is trivial, and the assembly compatibility is important. The existing `static_assert` infrastructure protects us.

### 3.5 Scheduling Policy Interface

A policy is a struct with:
1. A nested `rq_data` type — the policy's per-runqueue state.
2. Static member functions — the scheduling operations.

```cpp
// A valid scheduling policy must provide:
//
// struct my_policy {
//     struct rq_data { ... };
//
//     static void   init(rq_data& data);
//     static void   enqueue(rq_data& data, task* t);
//     static void   dequeue(rq_data& data, task* t);
//     static task*  pick_next(rq_data& data);
//     static void   tick(rq_data& data, task* t);
// };
//
// - init:      Called once per CPU during sched::init(). Initialize rq_data.
// - enqueue:   Add a READY task. Policy decides placement (tail for RR,
//              sorted for priority, by vruntime for CFS).
// - dequeue:   Remove a task that is no longer READY (blocked, dead, migrated).
// - pick_next: Select the best task to run next. Remove it from the policy's
//              data structure. Return nullptr if no tasks are ready.
// - tick:      Called from the timer ISR (future). Policy can update accounting,
//              set needs_resched flags, etc. No-op if the policy doesn't need it.
```

**Why documented convention instead of a formal concept?**

We *could* write a C++20 concept:
```cpp
template<typename P>
concept SchedulingPolicy = requires(typename P::rq_data& d, task* t) {
    P::init(d);
    P::enqueue(d, t);
    P::dequeue(d, t);
    { P::pick_next(d) } -> /* can't express -> task* without <concepts> */;
    P::tick(d, t);
};
```
But `<concepts>` isn't available in freestanding, and the concept header is needed for return-type constraints. Without it, the concept can only check that the expressions are valid, not that they return the right types. The compiler already gives a clear error if a required function is missing (the scheduler core calls it, so if it doesn't exist, compilation fails). A documented convention is sufficient and pragmatic.

### 3.6 Round-Robin Policy — `kernel/sched/round_robin.h`

```cpp
#ifndef STELLUX_SCHED_ROUND_ROBIN_H
#define STELLUX_SCHED_ROUND_ROBIN_H

#include "sched/list.h"
#include "sched/task.h"

namespace sched {

struct round_robin {

    // Per-runqueue state: a FIFO list of ready tasks.
    struct rq_data {
        list_node head;     // Circular list head (sentinel)
    };

    /**
     * @brief Initialize the round-robin runqueue data.
     * Sets up the sentinel node for the empty task list.
     */
    static void init(rq_data& data) {
        list_init(&data.head);
    }

    /**
     * @brief Enqueue a task at the tail (FIFO order).
     */
    static void enqueue(rq_data& data, task* t) {
        list_add_tail(&t->runq_node, &data.head);
    }

    /**
     * @brief Remove a task from the runqueue.
     */
    static void dequeue(rq_data& data, task* t) {
        (void)data; // list_remove is self-contained
        list_remove(&t->runq_node);
    }

    /**
     * @brief Pick the first task from the head (FIFO).
     * @return The next task to run, or nullptr if empty.
     */
    static task* pick_next(rq_data& data) {
        list_node* node = list_pop_front(&data.head);
        if (!node) return nullptr;
        return list_entry<task>(node, &task::runq_node);
    }

    /**
     * @brief Timer tick callback. No-op for round-robin.
     */
    static void tick(rq_data& data, task* t) {
        (void)data;
        (void)t;
    }
};

} // namespace sched

#endif // STELLUX_SCHED_ROUND_ROBIN_H
```

**Design notes:**
- Everything is `inline` (defined in the header). The compiler can inline these into the scheduler core — zero function call overhead.
- `rq_data` is just a list sentinel node. 16 bytes per CPU.
- `pick_next()` uses `list_entry<task>(node, &task::runq_node)` to get the `task*` from the list node.
- `tick()` is a no-op. When we evolve to a timeslice-aware round-robin, this would decrement the timeslice and set a reschedule flag.

### 3.7 Future CFS Policy — What It Would Look Like

```cpp
struct cfs {
    struct rq_data {
        rb_root tree;           // Red-black tree of tasks, keyed by vruntime
        uint64_t min_vruntime;  // Monotonically increasing floor
    };

    static void init(rq_data& data) {
        rb_init(&data.tree);
        data.min_vruntime = 0;
    }

    static void enqueue(rq_data& data, task* t) {
        // t->vruntime = max(t->vruntime, data.min_vruntime)
        // rb_insert(&data.tree, &t->cfs_node, by vruntime)
    }

    static task* pick_next(rq_data& data) {
        // rb_node* leftmost = rb_leftmost(&data.tree)
        // rb_remove(&data.tree, leftmost)
        // return container task
    }

    static void tick(rq_data& data, task* t) {
        // t->vruntime += delta_exec / t->load_weight
        // update data.min_vruntime
        // if (t->vruntime > leftmost->vruntime + threshold) set_needs_resched()
    }
};
```

The task struct would grow a `uint64_t vruntime` and an `rb_node cfs_node` field. The scheduler core doesn't change. The switch is:
```cpp
// In sched.cpp: change one line
using policy_t = sched::cfs;    // was: sched::round_robin
```

### 3.8 Runqueue — `kernel/sched/runqueue.h`

```cpp
#ifndef STELLUX_SCHED_RUNQUEUE_H
#define STELLUX_SCHED_RUNQUEUE_H

#include "sync/spinlock.h"
#include "sched/task.h"

namespace sched {

template<typename Policy>
struct runqueue {
    sync::spinlock lock;
    uint32_t nr_running;        // Number of READY tasks in this runqueue
    task* idle_task;             // Fallback task (never on the runqueue itself)
    typename Policy::rq_data data; // Policy-specific state (type-safe!)
};

} // namespace sched

#endif // STELLUX_SCHED_RUNQUEUE_H
```

**Design notes:**
- Templated on the policy type. `Policy::rq_data` is a concrete type, not an opaque byte array.
- For round-robin: `runqueue<round_robin>` contains `round_robin::rq_data { list_node head; }` — 16 bytes of policy data.
- For CFS: `runqueue<cfs>` would contain `cfs::rq_data { rb_root tree; uint64_t min_vruntime; }`.
- The per-CPU runqueue is: `DEFINE_PER_CPU(runqueue<policy_t>, cpu_runqueue);`

### 3.9 Scheduler Core — `kernel/sched/sched.h` (Public API)

```cpp
#ifndef STELLUX_SCHED_SCHED_H
#define STELLUX_SCHED_SCHED_H

#include "common/types.h"

// Forward declarations — callers don't need full task definition for most operations.
namespace sched {
    struct task;
    struct process;
}

namespace sched {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_NO_MEMORY   = -1;
constexpr int32_t ERR_INVALID_ARG = -2;

/**
 * @brief Initialize the scheduler subsystem.
 *
 * Sets up the BSP's per-CPU runqueue, registers the idle task,
 * and initializes the active scheduling policy.
 * Call after mm::init() and after sched::init_boot_task().
 *
 * @return OK on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Create a new kernel task within a new single-threaded process.
 *
 * Allocates a process, a task struct, a task stack, and a system stack.
 * Sets up the task's synthetic stack frame so the first context switch to it
 * enters the entry function via the trap return path.
 *
 * The task starts in task_state::created. Call enqueue() to make it schedulable.
 *
 * @param entry Function the task will execute. If it returns, sched::exit() is called.
 * @param arg   Opaque argument passed to entry (via RDI/x0).
 * @param name  Debug name (truncated to TASK_NAME_MAX - 1).
 * @return Pointer to the new task, or nullptr on allocation failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE task* create_kernel_task(
    void (*entry)(void*),
    void* arg,
    const char* name
);

/**
 * @brief Make a task schedulable.
 *
 * Transitions from task_state::created to task_state::ready and adds the task
 * to a runqueue via the active scheduling policy.
 *
 * @param t Task in task_state::created.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enqueue(task* t);

/**
 * @brief Voluntarily yield the CPU.
 *
 * Triggers a software interrupt (architecture-specific). The trap handler
 * calls schedule(), which moves the current task to the back of the runqueue
 * and switches to the next task.
 *
 * Can be called from any privilege level. The interrupt-routed context switch
 * saves and restores the caller's privilege level automatically.
 */
void yield();

/**
 * @brief Core scheduling function.
 *
 * Picks the next task via the active policy, performs a context switch.
 * Called from the trap handler (dispatched by yield's software interrupt
 * or a future timer interrupt). Not called directly by tasks.
 *
 * @note Privilege: **required** (runs in trap handler context)
 */
__PRIVILEGED_CODE void schedule();

/**
 * @brief Preemption entry point for timer ISR (future).
 *
 * Calls the policy's tick() to update accounting, then calls schedule()
 * if preemption is warranted. Currently a stub.
 *
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void reschedule();

/**
 * @brief Terminate the current task.
 *
 * Transitions to task_state::dead and calls schedule(). Does not return.
 * Resources are cleaned up lazily (future: by a reaper task or on join).
 *
 * @note Privilege: **required**
 */
[[noreturn]] __PRIVILEGED_CODE void exit();

/**
 * @brief Get the current task on this CPU.
 *
 * Recovers the full task* from the per-CPU task_exec_core* by offset math,
 * since task_exec_core is the first member of task.
 */
task* current();

} // namespace sched

#endif // STELLUX_SCHED_SCHED_H
```

**Design notes:**
- The header is **policy-agnostic**. No template parameters, no policy type. External code sees a clean namespace API.
- The policy type is an implementation detail of `sched.cpp`.
- `yield()` is NOT `__PRIVILEGED_CODE` — it can be called from unprivileged (lowered) code. The software interrupt handles the privilege transition.
- `schedule()` IS `__PRIVILEGED_CODE` — it runs inside the trap handler at Ring 0/EL1.
- `current()` is a lightweight function: the per-CPU `current_task` is a `task_exec_core*`, and since `exec` is the first member of `task`, we can cast directly.

### 3.10 Scheduler Core — `kernel/sched/sched.cpp` (Implementation Sketch)

```cpp
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/runqueue.h"
#include "sched/round_robin.h"      // ← change this line to swap policy
#include "sync/spinlock.h"
#include "percpu/percpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "common/logging.h"

namespace sched {

// ============================================================================
// Policy Selection (compile-time)
// ============================================================================

using policy_t = round_robin;       // ← and this line

// ============================================================================
// Per-CPU State
// ============================================================================

DEFINE_PER_CPU(runqueue<policy_t>, cpu_runqueue);

// ============================================================================
// ID Allocation
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
// Initialization
// ============================================================================

__PRIVILEGED_CODE int32_t init() {
    auto& rq = this_cpu(cpu_runqueue);
    rq.lock = sync::SPINLOCK_INIT;
    rq.nr_running = 0;

    policy_t::init(rq.data);

    // The boot task (already created by arch::early_init via init_boot_task)
    // becomes this CPU's idle task. Recover the task* from current_task.
    task* boot = current();
    boot->state = task_state::running;
    rq.idle_task = boot;

    log::info("sched: initialized (policy: %s)", policy_t::name);
    return OK;
}

// ============================================================================
// Task Creation
// ============================================================================

// Arch-specific: set up the synthetic stack so context_switch into this task
// returns through the trap exit path into the entry function.
extern void arch_setup_new_task(task* t, void (*entry)(void*), void* arg);

__PRIVILEGED_CODE task* create_kernel_task(
    void (*entry)(void*), void* arg, const char* name
) {
    // Allocate process
    process* proc = heap::kalloc_new<process>();
    if (!proc) return nullptr;
    proc->pid = alloc_pid();
    proc->thread_count = 1;
    list_init(&proc->task_list);
    proc->lock = sync::SPINLOCK_INIT;

    // Allocate task
    task* t = heap::kalloc_new<task>();
    if (!t) {
        heap::kfree_delete(proc);
        return nullptr;
    }

    t->tid = alloc_tid();
    t->proc = proc;
    t->state = task_state::created;
    t->priority = 0;

    // Copy name
    size_t i = 0;
    while (name[i] && i < TASK_NAME_MAX - 1) {
        t->name[i] = name[i];
        i++;
    }
    t->name[i] = '\0';

    // Allocate task stack (16KB + 1 guard page)
    uintptr_t stack_base, stack_top;
    if (vmm::alloc_stack(4, 1, kva::tag::privileged_stack,
                         stack_base, stack_top) != vmm::OK) {
        heap::kfree_delete(t);
        heap::kfree_delete(proc);
        return nullptr;
    }
    t->exec.task_stack_top = stack_top;

    // Allocate system stack (16KB + 1 guard page)
    uintptr_t sys_base, sys_top;
    if (vmm::alloc_stack(4, 1, kva::tag::privileged_stack,
                         sys_base, sys_top) != vmm::OK) {
        vmm::free(stack_base);
        heap::kfree_delete(t);
        heap::kfree_delete(proc);
        return nullptr;
    }
    t->exec.system_stack_top = sys_top;

    // Arch-specific: build synthetic stack frame (trap_frame + callee-saved)
    arch_setup_new_task(t, entry, arg);

    // Initialize exec core flags
    t->exec.flags = TASK_FLAG_KERNEL | TASK_FLAG_CAN_ELEVATE | TASK_FLAG_PREEMPTIBLE;
    t->exec.cpu = 0; // default to BSP

    // Link into process task list
    list_add_tail(&t->proc_node, &proc->task_list);

    log::debug("sched: created task '%s' tid=%u pid=%u", t->name, t->tid, proc->pid);
    return t;
}

// ============================================================================
// Enqueueing
// ============================================================================

__PRIVILEGED_CODE void enqueue(task* t) {
    auto& rq = this_cpu(cpu_runqueue);
    auto state = sync::spin_lock_irqsave(rq.lock);

    t->state = task_state::ready;
    policy_t::enqueue(rq.data, t);
    rq.nr_running++;

    sync::spin_unlock_irqrestore(rq.lock, state);
}

// ============================================================================
// Core Scheduling
// ============================================================================

// Arch-specific: save callee-saved regs of prev, switch stack, restore next.
extern "C" void context_switch(task_exec_core* prev, task_exec_core* next);

__PRIVILEGED_CODE void schedule() {
    auto& rq = this_cpu(cpu_runqueue);
    auto state = sync::spin_lock_irqsave(rq.lock);

    task* prev = current();

    // If the previous task is still running (i.e., it yielded voluntarily),
    // put it back on the runqueue. If it blocked or exited, its state is
    // already set and we don't re-enqueue it.
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

    if (prev == next) {
        // Nothing to switch to. Restore prev's state and return.
        prev->state = task_state::running;
        // Undo the enqueue if we just did it
        if (prev != rq.idle_task) {
            policy_t::dequeue(rq.data, prev);
            rq.nr_running--;
        }
        sync::spin_unlock_irqrestore(rq.lock, state);
        return;
    }

    next->state = task_state::running;
    next->exec.cpu = 0; // TODO: this_cpu_id() when SMP

    // Update the per-CPU current_task pointer.
    this_cpu(current_task) = &next->exec;

    sync::spin_unlock_irqrestore(rq.lock, state);

    // Perform the actual context switch (arch-specific assembly).
    context_switch(&prev->exec, &next->exec);

    // When `prev` is scheduled again, execution resumes here.
}

__PRIVILEGED_CODE void reschedule() {
    task* t = current();

    // Respect non-preemptible sections
    if (!(t->exec.flags & TASK_FLAG_PREEMPTIBLE))
        return;

    // Let the policy decide if preemption is needed
    auto& rq = this_cpu(cpu_runqueue);
    policy_t::tick(rq.data, t);

    // Future: check a needs_resched flag set by tick()
    // schedule();
}

// ============================================================================
// Task Termination
// ============================================================================

[[noreturn]] __PRIVILEGED_CODE void exit() {
    task* t = current();
    t->state = task_state::dead;

    // Decrement process thread count
    {
        sync::irq_lock_guard guard(t->proc->lock);
        t->proc->thread_count--;
    }

    log::debug("sched: task '%s' tid=%u exiting", t->name, t->tid);

    // schedule() will not re-enqueue us because state is dead.
    schedule();

    // Should never reach here.
    __builtin_unreachable();
}

// ============================================================================
// Queries
// ============================================================================

task* current() {
    task_exec_core* exec = this_cpu(current_task);
    // task_exec_core is the first member of task, so the pointers are equal.
    return reinterpret_cast<task*>(exec);
}

} // namespace sched
```

**Design discussion points:**

1. **`current()` and the `task_exec_core` relationship.** The per-CPU `current_task` is a `task_exec_core*` (needed by assembly — trap entry reads flags and stack at fixed offsets). Since `exec` is the first member of `task`, we can `reinterpret_cast<task*>(exec_ptr)` safely. The alternative would be to make `current_task` a `task*`, but then assembly would need to access `task.exec.flags` at a different offset, breaking the existing `static_assert` infrastructure.

2. **`schedule()` re-enqueue logic.** If `prev` is still `running`, it yielded voluntarily and goes back on the runqueue. If it's `blocked` or `dead`, it doesn't. This means `block()` (future) sets state to `blocked` *before* calling `schedule()`, and `exit()` sets state to `dead` before calling `schedule()`.

3. **Lock ordering.** The runqueue lock is held during `schedule()` but released *before* `context_switch()`. This is important — we can't hold a lock across a context switch (the lock is per-CPU, and the next task might be on a different CPU in the future). Actually, wait — is this correct? If we release the lock before context_switch, another CPU could modify the runqueue between unlock and switch. For single-CPU cooperative scheduling, this is fine (interrupts are disabled). For SMP, we'd need to rethink this. **This needs more thought.**

4. **`policy_t::name`** — The round-robin policy should have a `static constexpr const char* name = "round-robin";` for logging.

### 3.11 Architecture-Specific: Context Switch

This is pure assembly (one file per architecture). It's called from `schedule()` after the runqueue lock is released.

**x86_64 — `kernel/arch/x86_64/sched/context_switch.S` (sketch)**
```asm
.globl context_switch
.type context_switch, @function
context_switch:
    // rdi = prev (task_exec_core*)
    // rsi = next (task_exec_core*)

    // Save callee-saved registers of prev on the stack
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

    ret     // returns into next's saved context
```

**AArch64 — `kernel/arch/aarch64/sched/context_switch.S` (sketch)**
```asm
.globl context_switch
.type context_switch, %function
context_switch:
    // x0 = prev (task_exec_core*)
    // x1 = next (task_exec_core*)

    // Save callee-saved registers of prev
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!

    // Save prev's stack pointer
    mov x9, sp
    str x9, [x0, #TASK_STACK_OFFSET]

    // Restore next's stack pointer
    ldr x9, [x1, #TASK_STACK_OFFSET]
    mov sp, x9

    // Restore callee-saved registers of next
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16

    ret     // returns via x30 (LR)
```

**Note:** These save to the **system stack** (the stack used during trap handling). The trap_frame is below these callee-saved registers on the same stack. When `ret` returns into the trap handler's epilogue, the trap handler's exit code restores the trap_frame and does `iretq`/`eret`.

### 3.12 Architecture-Specific: New Task Stack Setup

`arch_setup_new_task()` builds the synthetic stack on the new task's system stack. This is arch-specific because the trap_frame layout differs.

**x86_64 — Synthetic stack layout:**
```
system_stack_top (growing downward):
    ┌────────────────────────┐
    │  hardware iret frame   │  SS, RSP, RFLAGS, CS, RIP
    │    RIP = entry_tramp   │
    │    CS = KERNEL_CS       │  (Ring 0 for kernel tasks)
    │    RSP = task_stack_top │
    │    RFLAGS = IF          │  (interrupts enabled)
    │    SS = KERNEL_DS       │
    ├────────────────────────┤
    │  error_code = 0        │
    │  vector = SCHED_VECTOR │
    ├────────────────────────┤
    │  trap_frame GPRs       │  R15..RAX (all zero except RDI = arg)
    ├────────────────────────┤
    │  Callee-saved frame    │  R15, R14, R13, R12, RBP, RBX (all zero)
    │  Return addr           │  → stlx_x86_trap_return (trap exit label)
    └────────────────────────┘  ← saved as exec.system_stack_top
```

When `context_switch()` switches to this task:
1. Pops callee-saved (all zero — don't matter)
2. `ret` → jumps to `stlx_x86_trap_return`
3. Trap exit restores the synthetic trap_frame (RDI gets `arg`)
4. `iretq` transitions to `entry_tramp` at Ring 0 with `task_stack_top` as RSP

**`entry_tramp` (task entry trampoline):**
```cpp
// Common code, not arch-specific
extern "C" void sched_task_entry_trampoline() {
    // At this point:
    // - RDI/x0 contains the argument (set by synthetic trap_frame)
    // - We're at the desired privilege level
    // - Interrupts are enabled
    //
    // The entry function address is stored somewhere accessible
    // (e.g., in the task struct, or passed via a second register).
    //
    // When the entry function returns, we call sched::exit().
    // This trampoline is never returned to.
}
```

**Open question:** How does the trampoline know which `entry` function to call? Options:
- **Store `entry` and `arg` in the task struct** and have the trampoline load them. The trampoline is a single global function that reads `sched::current()->entry_fn`.
- **Use two registers** in the synthetic trap_frame: RDI/x0 = arg, RSI/x1 = entry. The trampoline receives both.
- **Push a small frame** below the trap_frame with `entry` and `arg`, and have the trampoline pop them.

Simplest: add `entry` and `arg` fields to the task struct. The trampoline calls `sched::current()->entry(sched::current()->arg)`.

---

## 4. Interaction with Trap Infrastructure

### 4.1 x86_64 Dispatch

In `stlx_x86_64_trap_handler()`, add a check before the fatal handler:

```cpp
extern "C" __PRIVILEGED_CODE void stlx_x86_64_trap_handler(x86::trap_frame* tf) {
    sched::task_exec_core* task = this_cpu(current_task);
    task->flags |= sched::TASK_FLAG_IN_IRQ;

    if (tf->vector == x86::SCHED_VECTOR) {
        sched::schedule();
        task->flags &= ~sched::TASK_FLAG_IN_IRQ;
        return;
    }

    // ... existing fatal handler ...
    task->flags &= ~sched::TASK_FLAG_IN_IRQ;
}
```

### 4.2 AArch64 Dispatch

In the EL0 and EL1 sync handlers, add SVC dispatch for `SYS_YIELD`:

```cpp
extern "C" __PRIVILEGED_CODE
void stlx_aarch64_el0_sync_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;

    const uint64_t esr = tf->esr;
    const uint8_t ec = /* ... */;

    if (ec == aarch64::EC_SVC_A64) {
        uint16_t svc_num = static_cast<uint16_t>(esr & 0xFFFF);
        if (svc_num == syscall::SYS_YIELD) {
            sched::schedule();
            return;
        }
        // ... existing syscall dispatch ...
    }

    trap_fatal("el0 sync", tf);
}
```

### 4.3 Trap Exit Label

In `entry.S` (x86_64), add a label at the trap return point:

```asm
    call stlx_x86_64_trap_handler
    // ↓ New task bootstrap returns here via context_switch() → ret
.globl stlx_x86_trap_return
stlx_x86_trap_return:
    mov rsp, r12
    // ... existing trap exit code (restore trap_frame, swapgs, iretq) ...
```

Equivalent label in `vectors.S` for AArch64.

---

## 5. Component Dependency Graph

```
sched.h (public API)
    ↓ includes
task.h (task, process, task_state)
    ↓ includes
task_exec_core.h (existing, unchanged)
list.h (intrusive list)

sched.cpp (implementation)
    ↓ includes
sched.h
runqueue.h <Policy> (templated runqueue)
round_robin.h (policy implementation)
sync/spinlock.h
percpu/percpu.h
mm/heap.h, mm/vmm.h

arch/*/sched/context_switch.S (callee-saved switch)
arch/*/sched/sched.cpp (arch_setup_new_task, boot task init)
```

External code only includes `sched.h`. The policy, runqueue template, and list utilities are implementation details.

---

## 6. Summary: What Each File Contains

| File | Contains | Why Separate |
|------|----------|--------------|
| `sched/list.h` | Intrusive linked list | Reusable utility (runqueues, wait queues, process task lists) |
| `sched/task.h` | `task`, `process`, `task_state`, `TASK_NAME_MAX` | Data structure definitions — used by multiple components |
| `sched/runqueue.h` | `runqueue<Policy>` template | Per-CPU runqueue — templated on policy for type-safe policy data |
| `sched/sched.h` | Public API (`init`, `yield`, `schedule`, ...) | External interface — policy-agnostic |
| `sched/round_robin.h` | Round-robin policy (`rq_data`, `enqueue`, `pick_next`, ...) | Swappable scheduling algorithm |
| `sched/sched.cpp` | Scheduler core implementation | Core mechanics — calls policy through template |
| `arch/*/sched/context_switch.S` | Callee-saved register save/restore + stack switch | Per-architecture assembly |
| `arch/*/sched/sched.cpp` | `arch_setup_new_task()`, boot task init | Per-architecture task initialization |

---

## 7. Open Design Questions

### Q1: Lock Ordering in `schedule()`

The current sketch releases the runqueue lock *before* `context_switch()`. This is correct for single-CPU cooperative scheduling (interrupts are disabled inside the trap handler). But for SMP, there's a window between unlock and switch where another CPU could interfere. Linux handles this with a complex lock handoff (`rq->lock` is transferred from prev's CPU to next's CPU). We should document the intended SMP locking strategy even if we don't implement it now.

### Q2: Stack Pointer Field for Context Switch

The context switch saves/restores `task_exec_core.task_stack_top`. But during a trap-routed context switch, the callee-saved frame is on the **system stack**, not the task stack. Should we use `system_stack_top` instead? Or add a new field (`sched_sp`) for the scheduler's saved stack pointer?

Using `system_stack_top` seems right — during a trap-routed switch, we're on the system stack, so that's what we save/restore. But then `system_stack_top` serves double duty (initial system stack top for trap entry AND saved SP for context switch). These might need to be separate.

**Recommendation:** Add a dedicated `sched_sp` field (or repurpose `task_stack_top` during scheduling, since the task stack is captured in the trap_frame's RSP/SP field and doesn't need a separate save).

### Q3: Entry Function Storage

How does the task entry trampoline know which function to call? Simplest option: add `void (*entry_fn)(void*)` and `void* entry_arg` fields to the `task` struct. The trampoline reads them from `sched::current()`.

### Q4: Boot Task → Idle Task Transition

The existing boot task is a `task_exec_core` allocated as a static global. For the scheduler, we need a full `task` struct. Options:
- Allocate a `task` on the heap during `sched::init()` and copy the boot task's `exec` fields into it.
- Keep the boot task as a special static `task` (not heap-allocated).
- Make `init_boot_task()` allocate a full `task` instead of just a `task_exec_core`.

---

*This document is a living design discussion. Last updated to include detailed C++ component sketches, policy template design, and implementation-level pseudocode.*
