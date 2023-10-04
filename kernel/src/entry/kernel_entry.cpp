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
#include <sched/sched.h>
#include <syscall/syscalls.h>
#include <kprint.h>

EXTERN_C void _kentry(KernelEntryParams* params);
extern uint64_t __kern_phys_base;
extern Point g_cursorLocation;

extern uint64_t __ksymstart;
extern uint64_t __ksymend;

// Function prototype for the task function
typedef void (*task_function_t)();

// Function to create a task
PCB createTask(task_function_t task_function, uint64_t pid) {
    PCB newTask;

    // Initialize the PCB
    memset(&newTask, 0, sizeof(PCB));
    newTask.state = ProcessState::READY;
    newTask.pid = pid;
    newTask.priority = 0;

    // Allocate and initialize the stack
    void* stack = zallocPage();

    // Initialize the CPU context
    newTask.context.rsp = (uint64_t)((char*)stack + PAGE_SIZE);  // Point to the top of the stack
    newTask.context.rbp = newTask.context.rsp;                   // Point to the top of the stack
    newTask.context.rip = (uint64_t)task_function;               // Set instruction pointer to the task function
    newTask.context.rflags = 0x200;  // Enable interrupts
    newTask.cr3 = reinterpret_cast<uint64_t>(paging::getCurrentTopLevelPageTable());

    // Setup segment registers
    newTask.context.cs = __KERNEL_CS;
    newTask.context.ds = __KERNEL_DS;
    newTask.context.ss = newTask.context.ds;
    newTask.context.es = newTask.context.ds;
    newTask.context.fs = newTask.context.ds;
    newTask.context.gs = newTask.context.ds;

    return newTask;
}

PCB createUserspaceTask(task_function_t task_function, uint64_t pid) {
    PCB newTask;

    // Initialize the PCB
    memset(&newTask, 0, sizeof(PCB));
    newTask.state = ProcessState::READY;
    newTask.pid = pid;
    newTask.priority = 0;

    // Allocate both user and kernel stacks
    void* stack = zallocPage();
    void* kernelStack = zallocPage();

    // Allocate a separate page for the function to run on
    void* functionPage = zallocPage();
    memcpy(functionPage, (void*)task_function, PAGE_SIZE);

    // Initialize the CPU context
    newTask.context.rsp = (uint64_t)0x00007fffffffe000 + PAGE_SIZE;  // Point to the top of the stack
    newTask.context.rbp = newTask.context.rsp;  // Point to the top of the stack
    newTask.context.rip = 0x400000;  // Set instruction pointer to the task function
    newTask.context.rflags = 0x200;  // Enable interrupts

    // Set up segment registers for user space. These values correspond to the selectors in the GDT.
    newTask.context.cs = __USER_CS | 0x3;
    newTask.context.ds = __USER_DS | 0x3;
    newTask.context.es = newTask.context.ds;
    newTask.context.fs = newTask.context.ds;
    newTask.context.gs = newTask.context.ds;
    newTask.context.ss = newTask.context.ds;

    // Save the kernel stack
    newTask.kernelStack = (uint64_t)kernelStack + PAGE_SIZE;

    // Create a userspace page table
    paging::PageTable* userPml4 = paging::createUserspacePml4(paging::getCurrentTopLevelPageTable());
    paging::mapPage((void*)0x00007fffffffe000, (void*)__pa(stack), USERSPACE_PAGE, userPml4, paging::getGlobalPageFrameAllocator());
    paging::mapPage((void*)0x0000000000400000, (void*)__pa(functionPage), USERSPACE_PAGE, userPml4, paging::getGlobalPageFrameAllocator());

    newTask.cr3 = reinterpret_cast<uint64_t>(userPml4);

    return newTask;
}

EXTERN_C void userspace_function() {
    int64_t a = 1;
    char userStringBuffer[29] = { "This is a userspace message\n" };
    userStringBuffer[28] = '\0';
    uint64_t length = 29;

    while (a <= 1000000000) {
        ++a;

        if (a % 200000000 == 0) {
            unsigned long syscallNumber = SYSCALL_SYS_WRITE;
            unsigned long fd = 0;
            unsigned long _unused = 0;

            long ret;

            asm volatile(
                "mov %1, %%rax\n"  // syscall number
                "mov %2, %%rdi\n"  // arg1
                "mov %3, %%rsi\n"  // arg2
                "mov %4, %%rdx\n"  // arg3
                "mov %5, %%r10\n"  // arg4
                "mov %6, %%r8\n"   // arg5
                "syscall\n"
                "mov %%rax, %0\n"  // Capture return value
                : "=r"(ret)
                : "r"(syscallNumber), "r"(fd), "r"((uint64_t)userStringBuffer), "r"(length), "r"(_unused), "r"(_unused)
                : "rax", "rdi", "rsi", "rdx", "r10", "r8"
            );
            (void)ret;

            uint64_t cr3_value;
            __asm__ volatile(
                "mov %%cr3, %0"
                : "=r"(cr3_value) // Output operand
                :                 // No input operand
                :                 // No clobbered register
            );
        }
    }

    unsigned long syscallNumber = SYSCALL_SYS_EXIT;
    unsigned long _unused = 0;

    long ret;

    asm volatile(
        "mov %1, %%rax\n"  // syscall number
        "mov %2, %%rdi\n"  // arg1
        "mov %3, %%rsi\n"  // arg2
        "mov %4, %%rdx\n"  // arg3
        "mov %5, %%r10\n"  // arg4
        "mov %6, %%r8\n"   // arg5
        "syscall\n"
        "mov %%rax, %0\n"  // Capture return value
        : "=r"(ret)
        : "r"(syscallNumber), "r"(_unused), "r"(_unused), "r"(_unused), "r"(_unused), "r"(_unused)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8"
    );
    (void)ret;
}

int fibb(int n) {
    if (n == 0 || n == 1) {
        return n;
    }

    if (n == 30) {
        for (volatile int i = 0; i < 100000000; i++) {
            i = i + 1;
            i = i - 1;
        }
    }

    return fibb(n - 1) + fibb(n - 2);
}

// use recursive function to exercise context switch (fibb)
void simple_function() {
    while(1) {
        int result = fibb(36);
        kprint("simple_function> fibb(36): %i\n", result);
    }
}

void test_task_execution_and_preemption() {
    auto& sched = Scheduler::get();

    // Create some tasks and add them to the scheduler
    PCB task1, task2, task3;

    task1.state = ProcessState::RUNNING;
    task1.pid = 1;
    zeromem(&task1.context, sizeof(CpuContext));
    task1.context.rflags |= 0x200;

    task2 = createTask(simple_function, 2);
    task3 = createUserspaceTask(userspace_function, 3);

    sched.addTask(task1);
    sched.addTask(task2);
    sched.addTask(task3);
}

void getPageTableIndicesFromVirtualAddress(
    uint64_t vaddr,
    uint64_t* ipml4,
    uint64_t* ipdpt,
    uint64_t* ipdt,
    uint64_t* ipt
) {
    vaddr >>= 12;
    *ipt = vaddr & 0x1ff;
    vaddr >>= 9;
    *ipdt = vaddr & 0x1ff;
    vaddr >>= 9;
    *ipdpt = vaddr & 0x1ff;
    vaddr >>= 9;
    *ipml4 = vaddr & 0x1ff;
}

void _kentry(KernelEntryParams* params) {
    // Setup kernel stack
    uint64_t kernelStackTop = reinterpret_cast<uint64_t>(params->kernelStack) + PAGE_SIZE;
    asm volatile ("mov %0, %%rsp" :: "r"(kernelStackTop));

    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    initializeAndInstallGDT(params->kernelStack);

    // Immediately update the kernel physical base
    __kern_phys_base = reinterpret_cast<uint64_t>(params->kernelElfSegments[0].physicalBase);

    // Initialize serial port (for headless output)
    initializeSerialPort(SERIAL_PORT_BASE_COM1);

    // Initialize display and graphics context
    Display::initialize(&params->graphicsFramebuffer, params->textRenderingFont);

    paging::PageFrameAllocator& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();
    globalPageFrameAllocator.initializeFromMemoryMap(
        params->efiMemoryMap.base,
        params->efiMemoryMap.descriptorSize,
        params->efiMemoryMap.descriptorCount
    );

    // Lock pages with used resources
    globalPageFrameAllocator.lockPhysicalPage(params);
    globalPageFrameAllocator.lockPages(&__ksymstart, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPage(&params->textRenderingFont);
    globalPageFrameAllocator.lockPages(&params->kernelElfSegments, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPages(
        params->graphicsFramebuffer.base,
        params->graphicsFramebuffer.size / PAGE_SIZE + 1
    );

    // Update the root pml4 page table
    paging::g_kernelRootPageTable = paging::getCurrentTopLevelPageTable();
    
    // Set up the interrupt descriptor table and enable interrupts
    initializeAndInstallIdt();
    enableInterrupts();

    // Setup and enable syscalls
    enableSyscallInterface();

    kprintInfo("System total memory : %llu MB\n", globalPageFrameAllocator.getTotalSystemMemory() / 1024 / 1024);
    kprintInfo("System free memory  : %llu MB\n", globalPageFrameAllocator.getFreeSystemMemory() / 1024 / 1024);
    kprintInfo("System used memory  : %llu MB\n", globalPageFrameAllocator.getUsedSystemMemory() / 1024 / 1024);

    kprintInfo("The kernel is loaded at:\n");
    kprintInfo("    Physical : 0x%llx\n", (uint64_t)params->kernelElfSegments[0].physicalBase);
    kprintInfo("    Virtual  : 0x%llx\n\n", (uint64_t)params->kernelElfSegments[0].virtualBase);
    kprintInfo("KernelStack  : 0x%llx\n\n", (uint64_t)params->kernelStack);

    char vendorName[13];
    cpuid_readVendorId(vendorName);
    kprintInfo("CPU Vendor: %s\n", vendorName);
    kprintWarn("Is 5-level paging supported? %i\n\n", cpuid_isLa57Supported());

    // Initialize the scheduler
    auto& sched = Scheduler::get();
    sched.init();

    test_task_execution_and_preemption();

    // Initialize and identity mapping the base address of Local APIC
	initializeApic();
    configureApicTimerIrq(IRQ0);

    while(1) {
        int result = fibb(34);
        kprint("_kentry> fibb(34): %i\n", result);
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}
