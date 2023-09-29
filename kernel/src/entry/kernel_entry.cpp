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
#include <arch/x86/apic.h>
#include <sched/sched.h>
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

    // Allocate and initialize the stack
    void* stack = zallocPage();

    // Initialize the CPU context
    newTask.context.rsp = (uint64_t)((char*)stack + PAGE_SIZE);  // Point to the top of the stack
    newTask.context.rbp = newTask.context.rsp;                   // Point to the top of the stack
    newTask.context.rip = (uint64_t)task_function;               // Set instruction pointer to the task function
    newTask.context.rflags = 0x200;  // Enable interrupts

    return newTask;
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
    PCB task1, task2;

    task1.state = ProcessState::RUNNING;
    task1.pid = 1;
    zeromem(&task1.context, sizeof(CpuContext));
    task1.context.rflags |= 0x200;

    task2 = createTask(simple_function, 2);

    sched.addTask(task1);
    sched.addTask(task2);
}

void _kentry(KernelEntryParams* params) {
    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    intializeAndInstallGDT();

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

    // Setup kernel stack
    void* _globalKernelStack = zallocPage();
    uint64_t _kernelStackTop = reinterpret_cast<uint64_t>(_globalKernelStack) + PAGE_SIZE;
    asm volatile ("mov %0, %%rsp" :: "r"(_kernelStackTop));
    
    // Set up the interrupt descriptor table and enable interrupts
    initializeAndInstallIdt();
    enableInterrupts();

    kprintInfo("System total memory : %llu MB\n", globalPageFrameAllocator.getTotalSystemMemory() / 1024 / 1024);
    kprintInfo("System free memory  : %llu MB\n", globalPageFrameAllocator.getFreeSystemMemory() / 1024 / 1024);
    kprintInfo("System used memory  : %llu MB\n", globalPageFrameAllocator.getUsedSystemMemory() / 1024 / 1024);

    kprintInfo("The kernel is loaded at:\n");
    kprintInfo("    Physical : 0x%llx\n", (uint64_t)params->kernelElfSegments[0].physicalBase);
    kprintInfo("    Virtual  : 0x%llx\n\n", (uint64_t)params->kernelElfSegments[0].virtualBase);

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
