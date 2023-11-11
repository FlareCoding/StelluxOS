#include "gdt.h"
#include <memory/kmemory.h>
#include <arch/x86/msr.h>
#include <arch/x86/per_cpu_data.h>

EXTERN_C void __kinstall_gdt_asm(GdtDescriptor* descriptor);

__PRIVILEGED_DATA TaskStateSegment g_tss;
__PRIVILEGED_DATA GDT g_globalDescriptorTable;

__PRIVILEGED_DATA GdtSegmentDescriptor kernelNullDescriptor;
__PRIVILEGED_DATA GdtSegmentDescriptor kernelCodeDescriptor;
__PRIVILEGED_DATA GdtSegmentDescriptor kernelDataDescriptor;
__PRIVILEGED_DATA GdtSegmentDescriptor userCodeDescriptor;
__PRIVILEGED_DATA GdtSegmentDescriptor userDataDescriptor;

__PRIVILEGED_DATA TSSDescriptor        tssDescriptor;

__PRIVILEGED_DATA 
GdtDescriptor g_gdtDescriptor = {
    .limit = sizeof(GDT) - 1,
    .base = (uint64_t)&g_globalDescriptorTable
};

__PRIVILEGED_CODE
void setSegmentDescriptorBase(
    GdtSegmentDescriptor* descriptor,
    uint64_t base
) {
    descriptor->baseLow  = (base & 0xffff);
    descriptor->baseMid  = (base >> 16) & 0xFF;
    descriptor->baseHigh = (base >> 24) & 0xFF;
}

__PRIVILEGED_CODE
void setSegmentDescriptorLimit(
    GdtSegmentDescriptor* descriptor,
    uint64_t limit
) {
    // Set the lower 16 bits of the limit
    descriptor->limitLow = (uint16_t)(limit & 0xFFFF);

    // Set the higher 4 bits of the limit
    descriptor->limitHigh = (uint8_t)((limit >> 16) & 0xF);
}

__PRIVILEGED_CODE
void setTSSDescriptorBase(TSSDescriptor* desc, uint64_t base) {
    desc->baseLow  = (uint16_t)(base & 0xFFFF);
    desc->baseMid  = (uint8_t)((base >> 16) & 0xFF);
    desc->baseHigh = (uint8_t)((base >> 24) & 0xFF);
    desc->baseUpper = (uint32_t)((base >> 32) & 0xFFFFFFFF);
}

__PRIVILEGED_CODE
void setTSSDescriptorLimit(TSSDescriptor* desc, uint32_t limit) {
    desc->limitLow = (uint16_t)(limit & 0xFFFF);
    desc->limitHigh = (uint8_t)((limit >> 16) & 0x0F);
}

__PRIVILEGED_CODE
void initializeAndInstallGDT(void* kernelStack) {
    // Zero out all descriptors initially
    zeromem(&kernelNullDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&kernelCodeDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&kernelDataDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&userCodeDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&userDataDescriptor, sizeof(GdtSegmentDescriptor));
    
    // Initialize Kernel Code Segment
    setSegmentDescriptorBase(&kernelCodeDescriptor, 0);
    setSegmentDescriptorLimit(&kernelCodeDescriptor, 0xFFFFF);
    kernelCodeDescriptor.longMode = 1;
    kernelCodeDescriptor.granularity = 1;
    kernelCodeDescriptor.available = 1;
    kernelCodeDescriptor.accessByte.present = 1;
    kernelCodeDescriptor.accessByte.descriptorPrivilegeLvl = 0; // Kernel privilege level
    kernelCodeDescriptor.accessByte.executable = 1; // Code segment
    kernelCodeDescriptor.accessByte.readWrite = 1;
    kernelCodeDescriptor.accessByte.descriptorType = 1;

    // Initialize Kernel Data Segment
    setSegmentDescriptorBase(&kernelDataDescriptor, 0);
    setSegmentDescriptorLimit(&kernelDataDescriptor, 0xFFFFF);
    kernelDataDescriptor.longMode = 1;
    kernelDataDescriptor.granularity = 1;
    kernelDataDescriptor.available = 1;
    kernelDataDescriptor.accessByte.present = 1;
    kernelDataDescriptor.accessByte.descriptorPrivilegeLvl = 0; // Kernel privilege level
    kernelDataDescriptor.accessByte.executable = 0; // Data segment
    kernelDataDescriptor.accessByte.readWrite = 1;
    kernelDataDescriptor.accessByte.descriptorType = 1;
    
    // Initialize User Code Segment
    setSegmentDescriptorBase(&userCodeDescriptor, 0);
    setSegmentDescriptorLimit(&userCodeDescriptor, 0xFFFFF);
    userCodeDescriptor.longMode = 1;
    userCodeDescriptor.granularity = 1;
    userCodeDescriptor.available = 1;
    userCodeDescriptor.accessByte.present = 1;
    userCodeDescriptor.accessByte.descriptorPrivilegeLvl = 3; // Usermode privilege level
    userCodeDescriptor.accessByte.executable = 1; // Code segment
    userCodeDescriptor.accessByte.readWrite = 1;
    userCodeDescriptor.accessByte.descriptorType = 1;

    // Initialize User Data Segment
    setSegmentDescriptorBase(&userDataDescriptor, 0);
    setSegmentDescriptorLimit(&userDataDescriptor, 0xFFFFF);
    userDataDescriptor.longMode = 1;
    userDataDescriptor.granularity = 1;
    userDataDescriptor.available = 1;
    userDataDescriptor.accessByte.present = 1;
    userDataDescriptor.accessByte.descriptorPrivilegeLvl = 3; // Usermode privilege level
    userDataDescriptor.accessByte.executable = 0; // Data segment
    userDataDescriptor.accessByte.readWrite = 1;
    userDataDescriptor.accessByte.descriptorType = 1;

    // Initialize TSS
    zeromem(&g_tss, sizeof(TaskStateSegment));
    g_tss.rsp0 = reinterpret_cast<uint64_t>(kernelStack);
    g_tss.ioMapBase = sizeof(TaskStateSegment);

    // Initialize TSS descriptor
    setTSSDescriptorBase(&tssDescriptor, (uint64_t)&g_tss);
    setTSSDescriptorLimit(&tssDescriptor, sizeof(TaskStateSegment) - 1);
    tssDescriptor.accessByte.type = 0x9;  // 0b1001 for 64-bit TSS (Available)
    tssDescriptor.accessByte.present = 1;
    tssDescriptor.accessByte.dpl = 0; // Kernel privilege level
    tssDescriptor.accessByte.zero = 0; // Should be zero
    tssDescriptor.limitHigh = 0; // 64-bit TSS doesn't use limitHigh, set it to 0
    tssDescriptor.available = 1; // If you use this field, set it to 1
    tssDescriptor.granularity = 0; // No granularity for TSS
    tssDescriptor.zero = 0; // Should be zero
    tssDescriptor.zeroAgain = 0; // Should be zero

    // Update the GDT with initialized descriptors
    g_globalDescriptorTable.kernelNull = kernelNullDescriptor;
    g_globalDescriptorTable.kernelCode = kernelCodeDescriptor;
    g_globalDescriptorTable.kernelData = kernelDataDescriptor;
    g_globalDescriptorTable.userCode = userCodeDescriptor;
    g_globalDescriptorTable.userData = userDataDescriptor;
    g_globalDescriptorTable.tss = tssDescriptor;

    // Install the GDT
    __kinstall_gdt_asm(&g_gdtDescriptor);

    // Load the Task Register (TR)
    __asm__("ltr %%ax" : : "a" (__TSS_PT1_SELECTOR));

    __per_cpu_data.__cpu[BSP_CPU_ID].defaultKernelStack = reinterpret_cast<uint64_t>(kernelStack);

    // Store the address of the tss in gsbase
    writeMsr(IA32_GS_BASE, (uint64_t)&__per_cpu_data.__cpu[BSP_CPU_ID]);
}

__PRIVILEGED_CODE
TaskStateSegment* getActiveTSS() {
    return &g_tss;
}
