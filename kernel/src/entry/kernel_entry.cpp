#include "entry_params.h"
#include <memory/kmemory.h>
#include <graphics/kdisplay.h>
#include <gdt/gdt.h>
#include <paging/phys_addr_translation.h>
#include <paging/page.h>
#include <ports/serial.h>
#include <paging/tlb.h>
#include <interrupts/idt.h>
#include <arch/x86/cpuid.h>
#include <arch/x86/msr.h>
#include <arch/x86/apic.h>
#include <arch/x86/xsdt.h>
#include <arch/x86/per_cpu_data.h>
#include <sched/sched.h>
#include <syscall/syscalls.h>
#include <kelevate/kelevate.h>
#include <kprint.h>

EXTERN_C __PRIVILEGED_CODE void _kentry(KernelEntryParams* params);
extern uint64_t __kern_phys_base;

extern uint64_t __ksymstart;
extern uint64_t __ksymend;

KernelEntryParams g_entry_params;
char __usermode_kernel_entry_stack[0x8000];

// Function prototype for the task function
typedef void (*task_function_t)();

void _kuser_entry();
PCB createUserspaceTask(task_function_t task_function, uint64_t pid);
void testTaskExecutionAndPreemption();
int fibb(int n);

// use recursive function to exercise context switch (fibb)
void simple_function() {
    while(1) {
        int result = fibb(30);
        kuPrint("simple_function>  fibb(30): %i\n", result);
    }
}

void simple_function2() {
    while(1) {
        int result = fibb(28);
        kuPrint("simple_function2> fibb(28): %i\n", result);
    }
}

__PRIVILEGED_CODE void _kentry(KernelEntryParams* params) {
    // Setup kernel stack
    uint64_t kernelStackTop = reinterpret_cast<uint64_t>(params->kernelStack) + PAGE_SIZE;
    asm volatile ("mov %0, %%rsp" :: "r"(kernelStackTop));

    // Copy the kernel parameters to an unprivileged region
    memcpy(&g_entry_params, params, sizeof(KernelEntryParams));

    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    initializeAndInstallGDT(params->kernelStack);
    
    // Enable the syscall functionality
    enableSyscallInterface();

    // Immediately update the kernel physical base
    __kern_phys_base = reinterpret_cast<uint64_t>(params->kernelElfSegments[0].physicalBase);

    // Initialize serial port (for headless output)
    initializeSerialPort(SERIAL_PORT_BASE_COM1);

    // Initialize display and graphics context
    Display::initialize(&params->graphicsFramebuffer, params->textRenderingFont);

    char vendorName[13];
    cpuid_readVendorId(vendorName);
    kprintInfo("===== Stellux Kernel =====\n");
    kprintInfo("CPU Vendor: %s\n", vendorName);
    kprintWarn("Is 5-level paging supported? %i\n\n", cpuid_isLa57Supported());

    __call_lowered_entry(_kuser_entry, __usermode_kernel_entry_stack);
}

void _kuser_entry() {
    // Setup interrupts
    setup_interrupt_descriptor_table();

    RUN_ELEVATED({
        load_idtr();
        enableInterrupts();
    });

    // Setup page frame allocator and lock pages with used resources
    paging::PageFrameAllocator& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();
    
    RUN_ELEVATED({
        // Initialize the global page frame allocator
        globalPageFrameAllocator.initializeFromMemoryMap(
            g_entry_params.efiMemoryMap.base,
            g_entry_params.efiMemoryMap.descriptorSize,
            g_entry_params.efiMemoryMap.descriptorCount
        );

        // Update the root pml4 page table
        paging::g_kernelRootPageTable = paging::getCurrentTopLevelPageTable();
    });

    globalPageFrameAllocator.lockPage(&g_entry_params);
    globalPageFrameAllocator.lockPages(&__ksymstart, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPage(g_entry_params.textRenderingFont);
    globalPageFrameAllocator.lockPages(g_entry_params.kernelElfSegments, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPages(
        g_entry_params.graphicsFramebuffer.base,
        g_entry_params.graphicsFramebuffer.size / PAGE_SIZE + 1
    );

    kuPrint("System total memory : %llu MB\n", globalPageFrameAllocator.getTotalSystemMemory() / 1024 / 1024);
    kuPrint("System free memory  : %llu MB\n", globalPageFrameAllocator.getFreeSystemMemory() / 1024 / 1024);
    kuPrint("System used memory  : %llu MB\n", globalPageFrameAllocator.getUsedSystemMemory() / 1024 / 1024);

    kuPrint("The kernel is loaded at:\n");
    kuPrint("    Physical : 0x%llx\n", (uint64_t)__kern_phys_base);
    kuPrint("    Virtual  : 0x%llx\n\n", (uint64_t)&__ksymstart);
    kuPrint("KernelStack  : 0x%llx\n\n", (uint64_t)g_entry_params.kernelStack);

    initializeApic();
    configureApicTimerIrq(IRQ0);

    auto& sched = Scheduler::get();
    sched.init();

    // Initialize the default root kernel swapper task (this thread).
    // The CPU context should get properly filled upon the first context switch.
    PCB rootKernelSwapperTask;
    rootKernelSwapperTask.state = ProcessState::RUNNING;
    rootKernelSwapperTask.pid = 1;
    zeromem(&rootKernelSwapperTask.context, sizeof(CpuContext));
    rootKernelSwapperTask.context.rflags |= 0x200;
    rootKernelSwapperTask.elevated = 0;
    sched.addTask(rootKernelSwapperTask);

    // Add some sample tasks to test the scheduler code
    testTaskExecutionAndPreemption();

    // Infinite loop
    while (1) { __asm__ volatile("nop"); }
}

PCB createKernelTask(task_function_t task_function, uint64_t pid) {
    PCB newTask;
    zeromem(&newTask, sizeof(PCB));

    // Initialize the PCB
    memset(&newTask, 0, sizeof(PCB));
    newTask.state = ProcessState::READY;
    newTask.pid = pid;
    newTask.priority = 0;

    // Allocate both user and kernel stacks
    void* stack = zallocPage();
    void* kernelStack = zallocPage();

    // Initialize the CPU context
    newTask.context.rsp = (uint64_t)stack + PAGE_SIZE;  // Point to the top of the stack
    newTask.context.rbp = newTask.context.rsp;          // Point to the top of the stack
    newTask.context.rip = (uint64_t)task_function;      // Set instruction pointer to the task function
    newTask.context.rflags = 0x200;                     // Enable interrupts

    // Set up segment registers for user space. These values correspond to the selectors in the GDT.
    newTask.context.cs = __USER_CS | 0x3;
    newTask.context.ds = __USER_DS | 0x3;
    newTask.context.es = newTask.context.ds;
    newTask.context.fs = newTask.context.ds;
    newTask.context.gs = newTask.context.ds;
    newTask.context.ss = newTask.context.ds;

    // Save the kernel stack
    newTask.kernelStack = (uint64_t)kernelStack + PAGE_SIZE;

    // Setup the task's page table
    newTask.cr3 = reinterpret_cast<uint64_t>(paging::g_kernelRootPageTable);

    return newTask;
}

void testTaskExecutionAndPreemption() {
    auto& sched = Scheduler::get();

    // Create some tasks and add them to the scheduler
    PCB task1, task2;

    task1 = createKernelTask(simple_function, 2);
    task2 = createKernelTask(simple_function2, 3);

    sched.addTask(task1);
    sched.addTask(task2);
}

int fibb(int n) {
    if (n == 0 || n == 1) {
        return n;
    }

    // Extra time-consuming work
    // if (n == 30) {
    //     for (volatile int i = 0; i < 100000000; i++) {
    //         i = i + 1;
    //         i = i - 1;
    //     }
    // }

    return fibb(n - 1) + fibb(n - 2);
}
