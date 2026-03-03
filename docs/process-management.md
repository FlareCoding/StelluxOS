# Process Management Subsystem

## Status: Design Complete, Implementation Phase 1

---

## 1. Mental Model

### Tasks = Exec Core + Environment

Every schedulable entity in Stellux is a **task**. A task is composed of two
conceptually distinct parts:

1. **`task_exec_core`** — the CPU execution context: register state, stack
   pointers, FPU state, TLS base, page table roots (`pt_root`, `user_pt_root`),
   and the owning `mm_context` (VMAs, address space metadata). This is the
   "running program state."

2. **Environment** — everything else wrapped around the exec core: task ID,
   state machine (`CREATED`/`READY`/`RUNNING`/`BLOCKED`/`DEAD`), handle table
   (file descriptors, sockets, shared memory, **child processes**), scheduling
   links, cleanup/reaper state, and debug name.

The exec core is at offset 0 of the task struct (required for assembly
compatibility). The environment is the system-level wrapper.

### Why This Split Matters

The long-term vision is **snapshotable exec cores**. The idea:

- Snapshot (checkpoint) a task's exec core at a safe point.
- Construct multiple independent environments (different handle tables, different
  resource bindings).
- Stitch copies of the exec core into each environment.
- Schedule them.

**Example:** A pre-warmed server has its exec core ready to write to fd 4. You
snapshot it, create 10 environments where fd 4 points to 10 different client
sockets, stitch the exec core into each, and schedule them. All 10 send the same
response to different clients without re-executing any setup code.

This turns the OS into something like a **function-as-a-service platform** —
programs aren't just process binaries, they're checkpoint-able functions. But the
system also supports traditional complete process execution.

### Foundation First

The snapshotting/stitching layer is **not being built yet**. The current work
builds the foundational process management layer — creating, starting, waiting
for, and managing processes — designed so that snapshotting can be layered on top
later without rebuilding the foundation.

Key architectural decisions that preserve this path:

- `proc_create` and `proc_start` are separated, providing a configuration window
  between creation and scheduling where the environment can be manipulated.
- Processes are resources in the handle table, so the handle-based ownership
  model naturally extends to snapshotted exec cores later.
- The exec core / environment split is already enforced at the struct level.

---

## 2. Userland API

All process operations use **handle-based** references, not PIDs. Handles are
per-task, capability-scoped, and generation-tagged. Having a handle *is* the
authority to control the process.

### Functions

```c
#include <stlx/proc.h>

// Create a process from an ELF binary. Loads the ELF, creates a task in
// TASK_STATE_CREATED (not scheduled). Returns a handle on success, negative
// error code on failure.
int proc_create(const char* path, const char* argv[]);

// Convenience: proc_create + proc_start in one call. Returns handle on
// success, negative error code on failure.
// Implemented in libstlx (not a separate syscall).
int proc_exec(const char* path, const char* argv[]);

// Schedule a created (but not yet running) task. Returns 0 on success,
// negative error on failure.
int proc_start(int handle);

// Block until the child process exits. Stores the exit code in *exit_code.
// Consumes the handle (handle becomes invalid after this call — ownership
// is relinquished). Returns 0 on success.
int proc_wait(int handle, int* exit_code);

// Detach ownership of a child process. The handle becomes invalid. The
// child continues running independently; when it exits, the system reaper
// cleans it up. Returns 0 on success.
int proc_detach(int handle);

// Query process information. Handle must still be valid (call before
// proc_wait/proc_detach). Returns 0 on success.
int proc_info(int handle, process_info* info);
```

### Types

```c
typedef struct {
    char name[256];
    int pid;
    int cpu;
} process_info;
```

### Deferred Operations

The following are intentionally **out of scope** for the foundational layer:

- **`proc_kill`** — Requires signals or a similar notification primitive to do
  proper cleanup. Not blocking; can be added later.
- **`proc_dup_handle` / handle inheritance** — The ability to set up specific
  handles in the child's handle table before starting it (e.g., "set child's fd 4
  to this socket"). Essential for the snapshotting vision but deferred. Children
  currently start with an **empty handle table**.
- **`proc_suspend` / `proc_resume`** — Required for the snapshotting model.
  Deferred to the snapshotting layer.
- **Snapshotting / checkpointing** — The exec core snapshot, clone, and stitch
  operations. Deferred.

---

## 3. Ownership Semantics

### Parent Owns Children

When a process creates a child via `proc_create`, the child's process resource
is installed in the parent's handle table. The parent **owns** the child.

### Default: Parent Death Kills Children

If the parent task exits (or is killed), all resources in its handle table are
closed. For process-type resources, closing means:

- If the child hasn't been started: destroy it (unload ELF, free stacks).
- If the child is running: terminate it.
- If the child has already exited: free the task struct.

This is recursive — killing a child closes *its* handle table, which kills *its*
children.

This is intentionally different from Unix (where orphans get reparented to init).
Stellux's default prevents orphan accumulation. Daemon patterns are opt-in via
`proc_detach`.

### Explicit Detach for Daemons

`proc_detach(handle)` transfers ownership to the system reaper. The child
continues running independently. The parent's handle becomes invalid. When the
child eventually exits, the reaper cleans it up. No one collects the exit code.

### Zombie Semantics

If a child exits before the parent calls `proc_wait` or `proc_detach`, the child
is **not reaped** — the `proc_resource` stays alive with `exited = true` and the
exit code stored. The task's runtime resources (stacks, page tables) can be
reclaimed, but the proc_resource itself persists until the parent relinquishes
ownership.

This is similar to Unix zombies but naturally bounded: the proc_resource lives in
the parent's handle table (not a global list), and the handle is
generation-tagged (no PID recycling risk).

### proc_wait Consumes the Handle

`proc_wait` blocks until the child exits, returns the exit code, and
**relinquishes the handle** (the handle becomes invalid). This means:

- `proc_info` must be called **before** `proc_wait` if needed.
- After `proc_wait`, the child is fully reaped.
- The parent does not need to explicitly close the handle.

### Handle Inheritance

Children start with an **empty handle table**. No handles are inherited from the
parent. This is a deliberate capability-oriented design — the parent must
explicitly set up the child's resources.

For the foundational layer, there are no operations to pre-populate the child's
handle table. This will be added later (likely as `proc_dup_handle` or similar)
when needed for the snapshotting model and for standard I/O forwarding.

---

## 4. Kernel Implementation

### Process Resource Type

A new resource type `PROCESS = 4` in the `resource_type` enum.

The process resource provider (`kernel/resource/providers/proc_provider.h/.cpp`)
defines:

```cpp
struct proc_resource {
    sched::task*      child;      // the child task
    sync::wait_queue  wait_q;     // for proc_wait blocking
    int32_t           exit_code;  // stored on child exit
    bool              exited;     // true once child has terminated
    bool              detached;   // true if parent called proc_detach
};
```

Wrapped in a `resource_object` with `resource_ops`:

- `read` / `write`: return `ERR_UNSUP` (not a data stream).
- `close`: handles ownership release — terminate running child if not detached,
  destroy unstarted child, free exited child.

### Syscall Numbers

Stellux-specific syscall numbers (continuing from `SYS_ELEVATE = 1001`):

```
SYS_PROC_CREATE  = 1010
SYS_PROC_START   = 1011
SYS_PROC_WAIT    = 1012
SYS_PROC_DETACH  = 1013
SYS_PROC_INFO    = 1014
```

Registered in `kernel/syscall/syscall_table.cpp`. Handlers in
`kernel/syscall/handlers/sys_proc.h` / `sys_proc.cpp`.

### Task Exit Notification

The task exit path (`sched::exit`) is modified to notify waiters:

- The `task` struct gets a `proc_resource*` back-pointer (set during
  `proc_create`). This is environment metadata, not exec core.
- On exit: if the task has a `proc_resource`, store the exit code, set
  `exited = true`, call `sync::wake_all(proc_resource->wait_q)`.

### proc_create Flow

1. Copy path string from user space.
2. `exec::load_elf(path, &loaded)` — opens file from VFS, parses ELF, creates
   user page table, maps segments.
3. Set up child's user stack with ABI-conformant layout for musl's `_start`:
   `argc`, argv pointers, argv strings, envp=NULL, auxv=AT_NULL.
4. `sched::create_user_task(&loaded, name)` — task in `TASK_STATE_CREATED`.
5. `resource::init_task_handles(child_task)` — empty handle table.
6. Allocate `proc_resource`, set `child = task`, `exited = false`.
7. Wrap in `resource_object`, install in caller's handle table.
8. Set `child_task->proc_res = proc_resource` (back-pointer for exit notification).
9. Return handle.

### proc_start Flow

1. Resolve handle via `get_handle_object` with `resource_type::PROCESS`.
2. Get `proc_resource->child`.
3. Validate task is in `TASK_STATE_CREATED`.
4. `sched::enqueue(task)`.
5. Return 0.

### proc_wait Flow

1. Resolve handle to `proc_resource`.
2. If `exited == true`: skip to step 4.
3. Block on `proc_resource->wait_q` until woken.
4. Copy `exit_code` to user space.
5. Release/remove the handle from caller's handle table.
6. Return 0.

### proc_detach Flow

1. Resolve handle to `proc_resource`.
2. Set `detached = true`.
3. Remove handle from caller's handle table.
4. If child already exited: free task resources immediately.
5. If child still running: it continues; system reaper cleans up on exit.

### proc_info Flow

1. Resolve handle to `proc_resource`.
2. Read task name, tid, cpu from `proc_resource->child`.
3. Copy `process_info` to user space.

---

## 5. Userland Library: libstlx

### Purpose

`libstlx` provides Stellux-specific APIs that are not part of musl libc. It is
statically linked alongside musl into every userland application.

### Syscall Mechanism

libstlx reuses musl's `syscall()` function (from `<unistd.h>`) which already
handles the arch-specific register convention and instruction (`syscall` on
x86_64, `svc #0` on AArch64). Each libstlx function is a thin wrapper:

```c
#include <unistd.h>
#include <stlx/syscall_nums.h>

int proc_create(const char* path, const char* argv[]) {
    return (int)syscall(SYS_PROC_CREATE, path, argv);
}
```

### Directory Layout

```
userland/lib/libstlx/
├── include/
│   └── stlx/
│       ├── proc.h           # Process management API declarations
│       └── syscall_nums.h   # Stellux-specific syscall numbers
├── src/
│   └── proc.c               # Implementations (thin syscall wrappers)
└── Makefile                  # Builds libstlx.a
```

### Build Integration

- `libstlx.a` is built for both architectures.
- Headers installed to `userland/sysroot/<arch>/include/stlx/`.
- `userland/mk/app.mk` updated to link `-lstlx` into all apps.

### proc_exec is a libstlx Convenience, Not a Kernel Syscall

```c
int proc_exec(const char* path, const char* argv[]) {
    int handle = proc_create(path, argv);
    if (handle < 0) return handle;
    int err = proc_start(handle);
    if (err < 0) {
        close(handle);  // destroy unstarted process via normal handle close
        return err;
    }
    return handle;
}
```

---

## 6. Implementation Phases

### Phase 1: libstlx Scaffold + Syscall Number Space

- Create `userland/lib/libstlx/` with headers, source, Makefile.
- Define syscall numbers in both kernel (`syscall.h`) and userland
  (`syscall_nums.h`).
- Create empty `sys_proc` handler stubs returning `ENOSYS`.
- Register stubs in `syscall_table.cpp`.
- Update `app.mk` to link `-lstlx`.
- **Test:** init.c calls `proc_create(...)`, gets back `ENOSYS` — end-to-end
  plumbing works.

### Phase 2: Process Resource Type

- Add `PROCESS = 4` to `resource_type` enum.
- Create `kernel/resource/providers/proc_provider.h/.cpp` with `proc_resource`
  struct and `resource_ops`.
- **Test:** Compile-only.

### Phase 3: proc_create + proc_start Syscalls

- Implement `sys_proc_create`: copy path from user space, load ELF, create task,
  create proc_resource, install handle, return handle.
- Implement `sys_proc_start`: resolve handle, enqueue task.
- Set up minimal child stack: `argc=0, argv={NULL}, envp={NULL}` so musl's
  `_start` doesn't crash.
- Create a second test binary (`hello`) in the initrd.
- **Test:** init creates hello, starts it, hello prints and exits.

### Phase 4: Task Exit Notification + proc_wait

- Add `proc_resource*` back-pointer to `task` struct.
- Modify `sched::exit()` to store exit code and wake waiters.
- Implement `sys_proc_wait`: block on wait_q, copy exit code, release handle.
- **Test:** init creates hello, starts it, hello exits with code 42, init
  receives 42 from `proc_wait`.

### Phase 5: argv Passing

- In `sys_proc_create`, copy argv from parent user space and set up child stack
  with full System V ABI layout:
  ```
  [stack top]
    string data ("4\0", "38\0")
    AT_NULL (auxv terminator)
    NULL    (end of envp)
    NULL    (end of argv)
    argv[1] ptr
    argv[0] ptr
    argc     ← SP points here
  ```
- **Test:** init creates `add_calc` with argv `["4", "38"]`, child reads them,
  computes 4+38=42, exits with code 42, init receives 42.

### Phase 6: proc_detach + proc_info

- Implement `sys_proc_detach`: set detached, remove handle, orphan to reaper.
- Implement `sys_proc_info`: read task state, copy to user space.
- **Test:** Full `init.c` example works end-to-end.

### Phase 7: Parent Death Cleanup

- Process resource `close` handler terminates running children (if not detached).
- Existing `resource::close_all(task)` on task exit triggers this automatically.
- Recursive: child cleanup closes child's handle table, killing grandchildren.
- **Test:** Parent creates children, parent exits, verify children are terminated
  and resources freed.

---

## 7. Open Questions (For Future Reference)

### Address Space in the Snapshot Model

When snapshotting an exec core and stitching copies into multiple environments,
what happens to the address space? The exec core contains `mm_ctx`, `pt_root`,
`user_pt_root`. Options:

- **CoW (copy-on-write):** Share pages, copy on write. Cheap snapshots.
- **Full copy:** Independent deep copies. Expensive but simple.
- **Hybrid:** Share code pages, privatize data pages.

Decision deferred to the snapshotting layer.

### Identity After Cloning

If one exec core is cloned into 10 environments, each gets a different `tid`. But
the code inside may have cached its PID in TLS (`tls_base` is in exec_core). Does
`getpid()` return the new tid? Does TLS need re-initialization? Decision deferred.

### Safe Point Detection for Snapshotting

Snapshotting requires the task to be at a "safe point" (no in-flight syscalls,
descheduled). Is this enforced by the kernel (refuse to snapshot if
`TASK_FLAG_IN_SYSCALL` is set) or by userland (explicit yield to
snapshot-safe state)? Decision deferred.

### proc_wait Timeout / Non-blocking

The current `proc_wait` is purely blocking. Future extensions may include:

- Non-blocking poll (check without blocking).
- Timeout-bounded wait.
- Waiting for any child to exit (like `waitpid(-1)`).
- Integration with a general poll/epoll mechanism.

These can be added as new syscalls or flags to `proc_wait` without changing the
existing interface.

### Standard I/O Forwarding

Children currently start with an empty handle table. For standard I/O (stdin,
stdout, stderr), the parent will eventually need to set up handles in the child
before starting it. This requires `proc_dup_handle` or similar, deferred until
needed.

A possible future flag `INHERIT_STDIO` on `proc_create` was discussed but
deferred because standard I/O is not yet properly implemented.

---

## 8. Example Usage

```c
#include <stlx/proc.h>
#include <stdio.h>

int main(void) {
    const char* argv[] = { "4", "38", NULL };
    int handle = proc_create("/initrd/bin/add_calc", argv);
    if (handle < 0) {
        printf("init: Failed to create the process\r\n");
        return -1;
    }

    int err = proc_start(handle);
    if (err < 0) {
        printf("init: Failed to start the process\r\n");
        return -3;
    }

    printf("init: Started child process!\r\n");

    process_info info;
    err = proc_info(handle, &info);
    if (err < 0) {
        printf("init: Failed to get process info\r\n");
        return -5;
    }

    int exit_code = 0xfffff;
    err = proc_wait(handle, &exit_code);
    if (err < 0) {
        printf("init: Failed to wait for child process exit\r\n");
        return -7;
    }

    printf("init: Child process exited with code %i\r\n", exit_code);
    return 0;
}
```
