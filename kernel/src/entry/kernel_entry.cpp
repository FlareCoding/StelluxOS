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
#include <arch/x86/apic_timer.h>
#include <arch/x86/gsfsbase.h>
#include <sched/sched.h>
#include <syscall/syscalls.h>
#include <kelevate/kelevate.h>
#include <acpi/acpi_controller.h>
#include <kprint.h>
#include <kstring.h>
#include <kvector.h>
#include <time/ktime.h>

EXTERN_C __PRIVILEGED_CODE void _kentry(KernelEntryParams* params);
extern uint64_t __kern_phys_base;

extern uint64_t __ksymstart;
extern uint64_t __ksymend;

KernelEntryParams g_kernelEntryParameters;

#define USERMODE_KERNEL_ENTRY_STACK_SIZE 0x8000
char __usermodeKernelEntryStack[USERMODE_KERNEL_ENTRY_STACK_SIZE];

PCB g_rootKernelInitTask;

struct TestStruct {
    int x;
    int y;
    int z;
};

// Function prototype for the task function
typedef void (*task_function_t)();

void _kuser_entry();
void testTaskExecutionAndPreemption();
int fibb(int n);

// use recursive function to exercise context switch (fibb)
void simpleFunctionElevKprint() {
    __kelevate();
    while(1) {
        int result = fibb(32);
        kprint("simpleFunctionElevKprint>  fibb(32): %i\n", result);
    }
}

void simpleFunctionSyscallPrint() {
    __kelevate();
    while(1) {
        int result = fibb(36);
        (void)result;

        const char* msg = "simpleFunctionSyscallPrint> Calculated fibb(36)! Ignoring result...\n";
        __syscall(SYSCALL_SYS_WRITE, 0, (uint64_t)msg, strlen(msg), 0, 0, 0);
    }
}

void simpleFunctionKuprint() {
    __kelevate();
    while(1) {
        int result = fibb(34);
        kuPrint("simpleFunctionKuprint> fibb(34): %i\n", result);
    }
}

__PRIVILEGED_CODE void _kentry(KernelEntryParams* params) {
    // Setup kernel stack
    uint64_t kernelStackTop = reinterpret_cast<uint64_t>(params->kernelStack) + PAGE_SIZE;
    asm volatile ("mov %0, %%rsp" :: "r"(kernelStackTop));

    // Copy the kernel parameters to an unprivileged region
    memcpy(&g_kernelEntryParameters, params, sizeof(KernelEntryParams));

    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    initializeAndInstallGDT((void*)kernelStackTop);
    
    // Enable the syscall functionality
    enableSyscallInterface();

    // Immediately update the kernel physical base
    __kern_phys_base = reinterpret_cast<uint64_t>(params->kernelElfSegments[0].physicalBase);

    // Initialize serial ports (for headless output)
    initializeSerialPort(SERIAL_PORT_BASE_COM1);
    initializeSerialPort(SERIAL_PORT_BASE_COM2);
    initializeSerialPort(SERIAL_PORT_BASE_COM3);
    initializeSerialPort(SERIAL_PORT_BASE_COM4);

    // Initialize display and graphics context
    Display::initialize(&params->graphicsFramebuffer, params->textRenderingFont);

    char vendorName[13];
    cpuid_readVendorId(vendorName);
    kprintInfo("===== Stellux Kernel =====\n");
    kprintInfo("CPU Vendor: %s\n", vendorName);
    kprintWarn("Is 5-level paging supported? %i\n\n", cpuid_isLa57Supported());
    
    // Initialize the default root kernel swapper task (this thread).
    g_rootKernelInitTask.state = ProcessState::RUNNING;
    g_rootKernelInitTask.pid = 1;
    zeromem(&g_rootKernelInitTask.context, sizeof(CpuContext));
    g_rootKernelInitTask.context.rflags |= 0x200;
    g_rootKernelInitTask.elevated = 0;

    // Set the current task in the per cpu region
    __per_cpu_data.__cpu[BSP_CPU_ID].currentTask = &g_rootKernelInitTask;

    __call_lowered_entry(_kuser_entry, __usermodeKernelEntryStack + USERMODE_KERNEL_ENTRY_STACK_SIZE);
}

void _kuser_entry() {
    setupInterruptDescriptorTable();

    RUN_ELEVATED({
        loadIdtr();
        enableInterrupts();
    });

    // Setup page frame allocator and lock pages with used resources
    paging::PageFrameAllocator& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();
    
    RUN_ELEVATED({
        // Initialize the global page frame allocator
        globalPageFrameAllocator.initializeFromMemoryMap(
            g_kernelEntryParameters.efiMemoryMap.base,
            g_kernelEntryParameters.efiMemoryMap.descriptorSize,
            g_kernelEntryParameters.efiMemoryMap.descriptorCount
        );

        // Update the root pml4 page table
        paging::g_kernelRootPageTable = paging::getCurrentTopLevelPageTable();
    });

    globalPageFrameAllocator.lockPage(&g_kernelEntryParameters);
    globalPageFrameAllocator.lockPages(&__ksymstart, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPage(g_kernelEntryParameters.textRenderingFont);
    globalPageFrameAllocator.lockPages(g_kernelEntryParameters.kernelElfSegments, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPages(
        g_kernelEntryParameters.graphicsFramebuffer.base,
        g_kernelEntryParameters.graphicsFramebuffer.size / PAGE_SIZE + 1
    );

    RUN_ELEVATED({
        setMtrrWriteCombining((uint64_t)__pa(g_kernelEntryParameters.graphicsFramebuffer.base), g_kernelEntryParameters.graphicsFramebuffer.size);
    });

    kuPrint("System total memory : %llu MB\n", globalPageFrameAllocator.getTotalSystemMemory() / 1024 / 1024);
    kuPrint("System free memory  : %llu MB\n", globalPageFrameAllocator.getFreeSystemMemory() / 1024 / 1024);
    kuPrint("System used memory  : %llu MB\n", globalPageFrameAllocator.getUsedSystemMemory() / 1024 / 1024);

    kuPrint("The kernel is loaded at:\n");
    kuPrint("    Physical : 0x%llx\n", (uint64_t)__kern_phys_base);
    kuPrint("    Virtual  : 0x%llx\n\n", (uint64_t)&__ksymstart);
    kuPrint("KernelStack  : 0x%llx\n\n", (uint64_t)g_kernelEntryParameters.kernelStack + PAGE_SIZE);

    initializeApic();

    auto& acpiController = AcpiController::get();

    RUN_ELEVATED({
        acpiController.init(g_kernelEntryParameters.rsdp);
    });

    // Initialize high precision event timer and query hardware frequency
    KernelTimer::init();

    // Calibrate apic timer tickrate to 100 milliseconds
    KernelTimer::calibrateApicTimer(100);

    // Start the kernel-wide APIC periodic timer
    KernelTimer::startApicPeriodicTimer();

    if (acpiController.hasPciDeviceTable()) {
        auto pciDeviceTable = acpiController.getPciDeviceTable();
        
        size_t idx = pciDeviceTable->findXhciController();
        if (idx != kstl::npos) {
            auto& xhciControllerPciDeviceInfo = pciDeviceTable->getDeviceInfo(idx);
            dbgPrintPciDeviceInfo(&xhciControllerPciDeviceInfo.headerInfo);
        }
    }

    // if (acpiController.hasApicTable()) {
    //     auto apicTable = acpiController.getApic();
    //     kuPrint("==== Detect %lli CPUs ====\n", apicTable->getCpuCount());
    //     for (size_t i = 0; i < apicTable->getCpuCount(); ++i) {
    //         kuPrint("    Core %lli: online\n", apicTable->getLocalApicDescriptor(i).apicId);
    //     }
    // }

    auto& sched = RoundRobinScheduler::get();
 
    // Add the root kernel swapper task to the scheduler. The CPU context
    // should get properly filled upon the first context switch.
    sched.addTask(g_rootKernelInitTask);

    // Add some sample tasks to test the scheduler code
    //testTaskExecutionAndPreemption();

    while (true) {
        sleep(1);
        kuPrint(
            "Ticked: ns:%llu us:%llu ms:%llu s:%llu\n",
            KernelTimer::getSystemTimeInNanoseconds(),
            KernelTimer::getSystemTimeInMicroseconds(),
            KernelTimer::getSystemTimeInMilliseconds(),
            KernelTimer::getSystemTimeInSeconds()
        );
    }

    // Infinite loop
    while (1) { __asm__ volatile("nop"); }
}

PCB createKernelTask(task_function_t taskFunction, uint64_t pid) {
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
    newTask.context.rip = (uint64_t)taskFunction;       // Set instruction pointer to the task function
    newTask.context.rflags = 0x200;                     // Enable interrupts

    // Set up segment registers for user space. These values correspond to the selectors in the GDT.
    newTask.context.cs = __USER_CS | 0x3;
    newTask.context.ds = __USER_DS | 0x3;
    newTask.context.es = newTask.context.ds;
    newTask.context.ss = newTask.context.ds;

    // Save the kernel stack
    newTask.kernelStack = (uint64_t)kernelStack + PAGE_SIZE;

    // Setup the task's page table
    newTask.cr3 = reinterpret_cast<uint64_t>(paging::g_kernelRootPageTable);

    return newTask;
}

void testTaskExecutionAndPreemption() {
    auto& sched = RoundRobinScheduler::get();

    // Create some tasks and add them to the scheduler
    PCB task1, task2, task3;

    task1 = createKernelTask(simpleFunctionElevKprint, 2);
    task2 = createKernelTask(simpleFunctionSyscallPrint, 3);
    task3 = createKernelTask(simpleFunctionKuprint, 4);

    sched.addTask(task1);
    sched.addTask(task2);
    sched.addTask(task3);
}

int fibb(int n) {
    if (n == 0 || n == 1) {
        return n;
    }

    return fibb(n - 1) + fibb(n - 2);
}
