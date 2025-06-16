#include <process/process.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <arch/x86/gdt/gdt.h>
#include <sched/sched.h>
#include <dynpriv/dynpriv.h>
#include <process/elf/elf64_loader.h>
#include <core/klog.h>

DEFINE_PER_CPU(process*, current_process);
DEFINE_PER_CPU(process_core*, current_process_core);
DEFINE_PER_CPU(uint64_t, current_system_stack);

#define SCHED_STACK_TOP_PADDING     0x80

#define SCHED_SYSTEM_STACK_PAGES    2
#define SCHED_TASK_STACK_PAGES      8

#define SCHED_USERLAND_TASK_STACK_PAGES SCHED_TASK_STACK_PAGES
#define SCHED_USERLAND_TASK_STACK_SIZE  SCHED_USERLAND_TASK_STACK_PAGES * PAGE_SIZE - SCHED_STACK_TOP_PADDING

#define SCHED_SYSTEM_STACK_SIZE     SCHED_SYSTEM_STACK_PAGES * PAGE_SIZE - SCHED_STACK_TOP_PADDING
#define SCHED_TASK_STACK_SIZE       SCHED_TASK_STACK_PAGES * PAGE_SIZE - SCHED_STACK_TOP_PADDING

#define USERLAND_TASK_STACK_TOP     0x00007fffffffffff

namespace sched {
// Saves context registers from the interrupt frame into a CPU context struct
__PRIVILEGED_CODE
void save_cpu_context(ptregs* process_context, ptregs* irq_frame) {
    memcpy(process_context, irq_frame, sizeof(ptregs));
}

// Saves context registers from the CPU context struct into an interrupt frame
__PRIVILEGED_CODE
void restore_cpu_context(ptregs* process_context, ptregs* irq_frame) {
    memcpy(irq_frame, process_context, sizeof(ptregs));
}

// Saves and restores necessary registers into the appropriate
// process control blocks using an interrupt frame.
// *Note* Meant to be called from within an interrupt handler
// and context would get switched upon interrupt return.
__PRIVILEGED_CODE
void switch_context_in_irq(
    int old_cpu,
    int new_cpu,
    process* from,
    process* to,
    ptregs* irq_frame
) {
    __unused old_cpu;
    __unused new_cpu;

    // Get the process cores
    process_core* from_core = from->get_core();
    process_core* to_core = to->get_core();

    // Save the current context into the 'from' process core
    save_cpu_context(&from_core->cpu_context, irq_frame);

    // Save the current MMU context into the 'from' process core
    from_core->mm_ctx = save_mm_context();

    // Restore the context from the 'to' process core
    restore_cpu_context(&to_core->cpu_context, irq_frame);

    // Perform an address space switch if needed
    if (from_core->mm_ctx.root_page_table != to_core->mm_ctx.root_page_table) {
        install_mm_context(to_core->mm_ctx);
        memory_barrier();
    }

    // Set the new value of current_process for the current CPU
    this_cpu_write(current_process, to);

    // Set the new value of the current process's execution core
    this_cpu_write(current_process_core, to->get_core());

    // Set the new value of the current system stack
    this_cpu_write(current_system_stack, to_core->stacks.system_stack_top);
}

__PRIVILEGED_CODE
process_core* create_priv_kernel_process_core(task_entry_fn_t entry, void* process_data) {
    process_core* core = new process_core();
    if (!core) {
        return nullptr;
    }

    // Initialize the process core state
    core->state = process_state::READY;
    core->hw_state.elevated = 1;
    core->hw_state.cpu = 0;  // Will be set by scheduler when assigned
    core->identity.pid = alloc_process_id();

    // Allocate the primary execution task stack
    void* task_stack = vmm::alloc_contiguous_virtual_pages(SCHED_TASK_STACK_PAGES, DEFAULT_PRIV_PAGE_FLAGS);
    if (!task_stack) {
        delete core;
        return nullptr;
    }

    core->stacks.task_stack = reinterpret_cast<uint64_t>(task_stack);
    core->stacks.task_stack_top = core->stacks.task_stack + SCHED_TASK_STACK_SIZE;

    // Allocate the system stack used for sensitive system and interrupt contexts
    core->stacks.system_stack = allocate_system_stack(core->stacks.system_stack_top);
    if (!core->stacks.system_stack) {
        delete core;
        vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(task_stack), SCHED_TASK_STACK_PAGES);
        return nullptr;
    }

    // Initialize the CPU context
    core->cpu_context.hwframe.rip = reinterpret_cast<uint64_t>(entry);        // Set instruction pointer to the task function
    core->cpu_context.hwframe.rflags = 0x200;                                 // Enable interrupts
    core->cpu_context.hwframe.rsp = core->stacks.task_stack_top;              // Point to the top of the stack
    core->cpu_context.rbp = core->cpu_context.hwframe.rsp;                    // Point to the top of the stack
    core->cpu_context.rdi = reinterpret_cast<uint64_t>(process_data);         // Process parameter buffer pointer

    // Set up segment registers for kernel space. These values correspond to the selectors in the GDT.
    uint64_t data_segment = __KERNEL_DS;
    core->cpu_context.ds = data_segment;
    core->cpu_context.es = data_segment;
    core->cpu_context.hwframe.ss = data_segment;
    core->cpu_context.hwframe.cs = __KERNEL_CS;

    // Setup the page table
    core->mm_ctx.root_page_table = reinterpret_cast<uint64_t>(paging::get_pml4());

    return core;
}

__PRIVILEGED_CODE
process_core* create_unpriv_kernel_process_core(task_entry_fn_t entry, void* process_data) {
    process_core* core = new process_core();
    if (!core) {
        return nullptr;
    }

    // Initialize the process core state
    core->state = process_state::READY;
    core->hw_state.elevated = 0;  // Unprivileged mode
    core->hw_state.cpu = 0;       // Will be set by scheduler when assigned
    core->identity.pid = alloc_process_id();

    // Allocate the primary execution task stack
    void* task_stack = vmm::alloc_contiguous_virtual_pages(SCHED_TASK_STACK_PAGES, DEFAULT_UNPRIV_PAGE_FLAGS);
    if (!task_stack) {
        delete core;
        return nullptr;
    }

    core->stacks.task_stack = reinterpret_cast<uint64_t>(task_stack);
    core->stacks.task_stack_top = core->stacks.task_stack + SCHED_TASK_STACK_SIZE;

    // Allocate the system stack used for sensitive system and interrupt contexts
    core->stacks.system_stack = allocate_system_stack(core->stacks.system_stack_top);
    if (!core->stacks.system_stack) {
        delete core;
        vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(task_stack), SCHED_TASK_STACK_PAGES);
        return nullptr;
    }

    // Initialize the CPU context
    core->cpu_context.hwframe.rip = reinterpret_cast<uint64_t>(entry);        // Set instruction pointer to the task function
    core->cpu_context.hwframe.rflags = 0x200;                                 // Enable interrupts
    core->cpu_context.hwframe.rsp = core->stacks.task_stack_top;              // Point to the top of the stack
    core->cpu_context.rbp = core->cpu_context.hwframe.rsp;                    // Point to the top of the stack
    core->cpu_context.rdi = reinterpret_cast<uint64_t>(process_data);         // Process parameter buffer pointer

    // Set up segment registers for unprivileged kernel space. These values correspond to the selectors in the GDT.
    uint64_t data_segment = __USER_DS | 0x3;  // Add RPL=3 for user mode
    core->cpu_context.ds = data_segment;
    core->cpu_context.es = data_segment;
    core->cpu_context.hwframe.ss = data_segment;
    core->cpu_context.hwframe.cs = __USER_CS | 0x3;  // Add RPL=3 for user mode

    // Setup the page table
    core->mm_ctx.root_page_table = reinterpret_cast<uint64_t>(paging::get_pml4());

    return core;
}

__PRIVILEGED_CODE
process_core* create_userland_process_core(
    uintptr_t entry_addr,
    paging::page_table* pt
) {
    process_core* core = new process_core();
    if (!core) {
        return nullptr;
    }

    // Initialize the process core state
    core->state = process_state::READY;
    core->hw_state.elevated = 0;  // Userland processes are unprivileged by default
    core->hw_state.cpu = 0;       // Will be set by scheduler when assigned
    core->identity.pid = alloc_process_id();

    // Initialize VMA management for the process
    core->mm_ctx.root_page_table = reinterpret_cast<uint64_t>(pt);
    if (!init_process_vma(&core->mm_ctx)) {
        kprint("[VMA] Failed to initialize process VMA\n");
        delete core;
        return nullptr;
    }

    // Allocate the system stack used for sensitive system and interrupt contexts
    core->stacks.system_stack = allocate_system_stack(core->stacks.system_stack_top);
    if (!core->stacks.system_stack) {
        delete core;
        return nullptr;
    }

    // Map the userland task stack
    if (!map_userland_process_stack(pt, core->stacks.task_stack, core->stacks.task_stack_top)) {
        vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(core->stacks.system_stack), SCHED_SYSTEM_STACK_PAGES);
        delete core;
        return nullptr;
    }

    // Create VMA entry for the userland task stack
    vma_area* stack_vma = create_vma(
        &core->mm_ctx,
        core->stacks.task_stack,
        SCHED_TASK_STACK_SIZE,
        VMA_PROT_READ | VMA_PROT_WRITE,
        VMA_TYPE_PRIVATE
    );

    if (!stack_vma) {
        kprint("Failed to create stack VMA\n");
        vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(core->stacks.system_stack), SCHED_SYSTEM_STACK_PAGES);
        vmm::unmap_contiguous_virtual_pages(core->stacks.task_stack, SCHED_USERLAND_TASK_STACK_PAGES);
        delete core;
        return nullptr;
    }

    // Initialize the CPU context
    core->cpu_context.hwframe.rip = entry_addr;                     // Set instruction pointer to the task function
    core->cpu_context.hwframe.rflags = 0x200;                       // Enable interrupts
    core->cpu_context.hwframe.rsp = core->stacks.task_stack_top;    // Point to the top of the stack
    core->cpu_context.rbp = core->cpu_context.hwframe.rsp;          // Point to the top of the stack

    // Set up segment registers for userland space. These values correspond to the selectors in the GDT.
    uint64_t data_segment = __USER_DS | 0x3;  // Add RPL=3 for user mode
    core->cpu_context.ds = data_segment;
    core->cpu_context.es = data_segment;
    core->cpu_context.hwframe.ss = data_segment;
    core->cpu_context.hwframe.cs = __USER_CS | 0x3;  // Add RPL=3 for user mode

    return core;
}

__PRIVILEGED_CODE
bool destroy_process_core(process_core* core) {
    if (!core) {
        return false;
    }

    // Free the system stack
    if (core->stacks.system_stack) {
        vmm::unmap_contiguous_virtual_pages(
            reinterpret_cast<uintptr_t>(core->stacks.system_stack),
            SCHED_SYSTEM_STACK_PAGES
        );
    }

    return true;

    // Free the task stack if it exists for kernel processes
    // using the kernel's virtual memory manager (vmm).
    if (core->stacks.task_stack && core->stacks.task_stack > USERLAND_TASK_STACK_TOP) {
        vmm::unmap_contiguous_virtual_pages(
            core->stacks.task_stack,
            SCHED_TASK_STACK_PAGES
        );
    }

    // Clean up VMA resources by removing all VMAs and unmapping their pages
    vma_area* vma = core->mm_ctx.vma_list;
    while (vma) {
        vma_area* next = vma->next;

        // Unmap only the userland pages for this VMA range
        if (vma->start < USERLAND_TASK_STACK_TOP) {
            size_t size = vma->end - vma->start;
            size_t num_pages = size / PAGE_SIZE;

            // Unmap pages and free physical memory
            for (size_t i = 0; i < num_pages; i++) {
                uintptr_t page_addr = vma->start + (i * PAGE_SIZE);
                void* phys_addr = reinterpret_cast<void*>(paging::get_physical_address(reinterpret_cast<void*>(page_addr)));
                
                if (phys_addr) {
                    // Unmap the page using the process's page table
                    paging::unmap_page(page_addr, reinterpret_cast<paging::page_table*>(core->mm_ctx.root_page_table));

                    // Free the physical page
                    allocators::page_bitmap_allocator::get_physical_allocator().free_page(phys_addr);
                }
            }
        }

        remove_vma(&core->mm_ctx, vma);
        vma = next;
    }

    //
    // TO-DO:
    //   - properly free up and deallocate the page table
    //   - deallocate the dynpriv whitelist entry if it exists
    //

    // Free the process core itself
    delete core;

    return true;
}

__PRIVILEGED_CODE
uint64_t allocate_system_stack(uint64_t& out_stack_top) {
    void* stack = vmm::alloc_linear_mapped_persistent_pages(SCHED_SYSTEM_STACK_PAGES);
    if (!stack) {
        return 0;
    }

    out_stack_top = reinterpret_cast<uint64_t>(stack) + SCHED_TASK_STACK_SIZE;
    return reinterpret_cast<uint64_t>(stack);
}

__PRIVILEGED_CODE bool map_userland_process_stack(
    paging::page_table* pt,
    uint64_t& out_stack_bottom,
    uint64_t& out_stack_top
) {
    const uint64_t user_stack_address_top = USERLAND_TASK_STACK_TOP;
    const uintptr_t user_stack_start_page =
        PAGE_ALIGN_UP(user_stack_address_top) - (SCHED_USERLAND_TASK_STACK_PAGES * PAGE_SIZE);

    auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
    void* phys_stack_start_page = physalloc.alloc_pages(SCHED_USERLAND_TASK_STACK_PAGES);
    if (!phys_stack_start_page) {
        return false;
    }

    paging::map_pages(
        user_stack_start_page,
        reinterpret_cast<uintptr_t>(phys_stack_start_page),
        SCHED_USERLAND_TASK_STACK_PAGES,
        DEFAULT_UNPRIV_PAGE_FLAGS,
        pt
    );

    out_stack_bottom = user_stack_start_page;
    out_stack_top = user_stack_start_page + SCHED_USERLAND_TASK_STACK_SIZE;

    return true;
}

void exit_process() {
    // The process needs to be elevated in order to call scheduler functions
    if (!dynpriv::is_elevated()) {
        dynpriv::elevate();
    }

    auto& scheduler = sched::scheduler::get();

    // First disable the timer IRQs to avoid bugs
    // that could come from an unexpected context
    // switch here.
    scheduler.preempt_disable();

    // Get the current process's core
    process_core* core = current->get_core();
    if (core) {
        // Indicate that this process is ready to be reaped
        core->state = process_state::TERMINATED;
    }

    // Release the process's reference to itself
    current->release_ref();

    // Add the process to the cleanup queue if needed
    if (core->ctx_switch_state.needs_cleanup == 1) {
        sched::scheduler::get().add_to_cleanup_queue(current);
    }

    // Remove the process from the scheduler queue
    scheduler.remove_process(current);

    // Trigger a context switch to switch to the next
    // available process in the scheduler run queue.
    scheduler.schedule();
}

void yield() {
    RUN_ELEVATED({
        // Trigger a context switch to switch to the next
        // available task in the scheduler run queue.
        sched::scheduler::get().schedule();
    });
}

} // namespace sched

__PRIVILEGED_CODE
process_core* process::_create_process_core(task_entry_fn_t entry, void* data, process_creation_flags flags) {
    // Create appropriate core based on flags
    if (has_process_flag(flags, process_creation_flags::IS_KERNEL)) {
        if (has_process_flag(flags, process_creation_flags::PRIV_KERN_THREAD)) {
            return sched::create_priv_kernel_process_core(entry, data);
        } else {
            return sched::create_unpriv_kernel_process_core(entry, data);
        }
    } else {
        // For userland processes, we need a page table
        auto pt = paging::create_higher_class_userland_page_table();
        if (!pt) {
            return nullptr;
        }
        auto core = sched::create_userland_process_core(
            reinterpret_cast<uintptr_t>(entry),
            pt
        );

        // Allow certain processes to elevate
        if (core && has_process_flag(flags, process_creation_flags::CAN_ELEVATE)) {
            dynpriv::whitelist_asid(core->mm_ctx.root_page_table);
        }

        return core;
    }
}

void process::add_ref() {
    m_ref_count.fetch_add(1);
#ifdef ENABLE_PROC_LIFECYCLE_TRACES
    kprint("pid:%u '%s'->add_ref() new_count:%u\n", m_core->identity.pid, m_core->identity.name, m_ref_count.load());
#endif
}

bool process::release_ref() {
    // Use atomic fetch_sub to decrement the reference count
    uint64_t old_count = m_ref_count.fetch_sub(1);
    
    // If this was the last reference
    if (old_count == 1) {
        // Indicate to the caller that this process needs
        // to be added to the scheduler's cleanup queue.
        m_core->ctx_switch_state.needs_cleanup = 1;
        memory_barrier();
    }
#ifdef ENABLE_PROC_LIFECYCLE_TRACES
    kprint("pid:%u '%s'->release_ref() new_count:%u\n", m_core->identity.pid, m_core->identity.name, m_ref_count.load());
#endif
    return true;
}

uint64_t process::get_ref_count() const {
    return m_ref_count.load();
}

__PRIVILEGED_CODE
bool process::init_with_flags(const char* name, process_creation_flags flags) {
    if (m_is_initialized) {
        return false;
    }

    // Create new environment
    m_env = new process_env();
    if (!m_env) {
        return false;
    }
    m_owns_env = true;
    m_env->creation_flags = flags;

    // Create core
    m_core = _create_process_core(nullptr, nullptr, flags);
    if (!m_core) {
        delete m_env;
        m_env = nullptr;
        return false;
    }
    m_owns_core = true;

    // Set process name
    strcpy(m_core->identity.name, name);
    m_core->identity.name[MAX_PROCESS_NAME_LEN] = '\0';

    m_is_initialized = true;

    // Schedule the process if requested
    if (has_process_flag(flags, process_creation_flags::SCHEDULE_NOW)) {
        sched::scheduler::get().add_process(this);
    }

    return true;
}

__PRIVILEGED_CODE
bool process::init_with_core(process_core* core, process_creation_flags flags, bool take_ownership) {
    if (m_is_initialized || !core) {
        return false;
    }

    // Create new environment
    m_env = new process_env();
    if (!m_env) {
        return false;
    }
    m_owns_env = true;
    m_env->creation_flags = flags;

    m_core = core;
    m_owns_core = take_ownership;

    // Allow authorized process to use dynamic privilege functionality
    if (has_process_flag(flags, process_creation_flags::CAN_ELEVATE)) {
        dynpriv::whitelist_asid(core->mm_ctx.root_page_table);
    }

    m_is_initialized = true;

    // Schedule the process if requested
    if (has_process_flag(flags, process_creation_flags::SCHEDULE_NOW)) {
        sched::scheduler::get().add_process(this);
    }

    return true;
}

__PRIVILEGED_CODE
bool process::init_with_env(const char* name, process_env* env, process_creation_flags flags, bool take_ownership) {
    if (m_is_initialized || !env) {
        return false;
    }

    m_env = env;
    m_owns_env = take_ownership;

    // Create core
    m_core = _create_process_core(nullptr, nullptr, flags);
    if (!m_core) {
        return false;
    }
    m_owns_core = true;

    // Set process name
    strcpy(m_core->identity.name, name);
    m_core->identity.name[MAX_PROCESS_NAME_LEN] = '\0';

    m_is_initialized = true;

    // Schedule the process if requested
    if (has_process_flag(flags, process_creation_flags::SCHEDULE_NOW)) {
        sched::scheduler::get().add_process(this);
    }

    return true;
}

__PRIVILEGED_CODE
bool process::init(process_core* core, bool take_core_ownership, process_env* env, bool take_env_ownership) {
    if (m_is_initialized || !core || !env) {
        return false;
    }

    m_core = core;
    m_owns_core = take_core_ownership;

    m_env = env;
    m_owns_env = take_env_ownership;

    m_is_initialized = true;

    // Schedule the process if requested
    if (has_process_flag(env->creation_flags, process_creation_flags::SCHEDULE_NOW)) {
        sched::scheduler::get().add_process(this);
    }

    return true;
}

__PRIVILEGED_CODE
bool process::init_with_entry(const char* name, task_entry_fn_t entry, void* data, process_creation_flags flags) {
    if (m_is_initialized || !entry) {
        return false;
    }

    // Create new environment
    m_env = new process_env();
    if (!m_env) {
        return false;
    }
    m_owns_env = true;
    m_env->creation_flags = flags;

    // Create core
    m_core = _create_process_core(entry, data, flags);
    if (!m_core) {
        delete m_env;
        m_env = nullptr;
        return false;
    }
    m_owns_core = true;

    // Set process name
    strcpy(m_core->identity.name, name);
    m_core->identity.name[MAX_PROCESS_NAME_LEN] = '\0';

    m_is_initialized = true;

    // Schedule the process if requested
    if (has_process_flag(flags, process_creation_flags::SCHEDULE_NOW)) {
        sched::scheduler::get().add_process(this);
    }

    return true;
}

__PRIVILEGED_CODE
bool process::init_with_entry(const char* name, task_entry_fn_t entry, void* data, process_env* env, process_creation_flags flags, bool take_ownership) {
    if (m_is_initialized || !entry || !env) {
        return false;
    }

    m_env = env;
    m_owns_env = take_ownership;

    // Create core
    m_core = _create_process_core(entry, data, flags);
    if (!m_core) {
        return false;
    }
    m_owns_core = true;

    // Set process name
    strcpy(m_core->identity.name, name);
    m_core->identity.name[MAX_PROCESS_NAME_LEN] = '\0';

    m_is_initialized = true;

    // Schedule the process if requested
    if (has_process_flag(flags, process_creation_flags::SCHEDULE_NOW)) {
        sched::scheduler::get().add_process(this);
    }

    return true;
}

__PRIVILEGED_CODE
void process::cleanup() {
    if (!m_is_initialized) {
        return;
    }

    // If we own the core, destroy it
    if (m_owns_core && m_core) {
        sched::destroy_process_core(m_core);
        m_core = nullptr;
    }

    // If we own the environment, delete it
    if (m_owns_env && m_env) {
        delete m_env;
        m_env = nullptr;
    }

    m_is_initialized = false;
    m_owns_core = false;
    m_owns_env = false;
}

bool process::start() {
    if (!m_is_initialized || !m_core) {
        return false;
    }

    m_core->state = process_state::READY;
    return true;
}

bool process::pause() {
    if (!m_is_initialized || !m_core) {
        return false;
    }

    m_core->state = process_state::WAITING;
    return true;
}

bool process::resume() {
    if (!m_is_initialized || !m_core) {
        return false;
    }

    m_core->state = process_state::READY;
    return true;
}

bool process::terminate() {
    if (!m_is_initialized || !m_core) {
        return false;
    }

    m_core->state = process_state::TERMINATED;
    return true;
}
