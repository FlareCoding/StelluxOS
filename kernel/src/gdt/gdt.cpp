#include "gdt.h"
#include <memory/kmemory.h>
#include <arch/x86/msr.h>
#include <arch/x86/per_cpu_data.h>

EXTERN_C void __kinstall_gdt_asm(GdtDescriptor* descriptor);

struct GdtAndTssData {
    GdtSegmentDescriptor kernelNullDescriptor;
    GdtSegmentDescriptor kernelCodeDescriptor;
    GdtSegmentDescriptor kernelDataDescriptor;
    GdtSegmentDescriptor userCodeDescriptor;
    GdtSegmentDescriptor userDataDescriptor;
    TSSDescriptor        tssDescriptor;

    GDT gdt;
    TaskStateSegment tss;

    GdtDescriptor gdtDescriptor;
};

__PRIVILEGED_DATA
GdtAndTssData g_gdtPerCpuArray[MAX_CPUS];

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
void initializeAndInstallGDT(int apicid, void* kernelStack) {
    GdtAndTssData* data = &g_gdtPerCpuArray[apicid];

    // Zero out all descriptors initially
    zeromem(&data->kernelNullDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&data->kernelCodeDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&data->kernelDataDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&data->userCodeDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&data->userDataDescriptor, sizeof(GdtSegmentDescriptor));
    
    // Initialize Kernel Code Segment
    setSegmentDescriptorBase(&data->kernelCodeDescriptor, 0);
    setSegmentDescriptorLimit(&data->kernelCodeDescriptor, 0xFFFFF);
    data->kernelCodeDescriptor.longMode = 1;
    data->kernelCodeDescriptor.granularity = 1;
    data->kernelCodeDescriptor.available = 1;
    data->kernelCodeDescriptor.accessByte.present = 1;
    data->kernelCodeDescriptor.accessByte.descriptorPrivilegeLvl = 0; // Kernel privilege level
    data->kernelCodeDescriptor.accessByte.executable = 1; // Code segment
    data->kernelCodeDescriptor.accessByte.readWrite = 1;
    data->kernelCodeDescriptor.accessByte.descriptorType = 1;

    // Initialize Kernel Data Segment
    setSegmentDescriptorBase(&data->kernelDataDescriptor, 0);
    setSegmentDescriptorLimit(&data->kernelDataDescriptor, 0xFFFFF);
    data->kernelDataDescriptor.longMode = 1;
    data->kernelDataDescriptor.granularity = 1;
    data->kernelDataDescriptor.available = 1;
    data->kernelDataDescriptor.accessByte.present = 1;
    data->kernelDataDescriptor.accessByte.descriptorPrivilegeLvl = 0; // Kernel privilege level
    data->kernelDataDescriptor.accessByte.executable = 0; // Data segment
    data->kernelDataDescriptor.accessByte.readWrite = 1;
    data->kernelDataDescriptor.accessByte.descriptorType = 1;
    
    // Initialize User Code Segment
    setSegmentDescriptorBase(&data->userCodeDescriptor, 0);
    setSegmentDescriptorLimit(&data->userCodeDescriptor, 0xFFFFF);
    data->userCodeDescriptor.longMode = 1;
    data->userCodeDescriptor.granularity = 1;
    data->userCodeDescriptor.available = 1;
    data->userCodeDescriptor.accessByte.present = 1;
    data->userCodeDescriptor.accessByte.descriptorPrivilegeLvl = 3; // Usermode privilege level
    data->userCodeDescriptor.accessByte.executable = 1; // Code segment
    data->userCodeDescriptor.accessByte.readWrite = 1;
    data->userCodeDescriptor.accessByte.descriptorType = 1;

    // Initialize User Data Segment
    setSegmentDescriptorBase(&data->userDataDescriptor, 0);
    setSegmentDescriptorLimit(&data->userDataDescriptor, 0xFFFFF);
    data->userDataDescriptor.longMode = 1;
    data->userDataDescriptor.granularity = 1;
    data->userDataDescriptor.available = 1;
    data->userDataDescriptor.accessByte.present = 1;
    data->userDataDescriptor.accessByte.descriptorPrivilegeLvl = 3; // Usermode privilege level
    data->userDataDescriptor.accessByte.executable = 0; // Data segment
    data->userDataDescriptor.accessByte.readWrite = 1;
    data->userDataDescriptor.accessByte.descriptorType = 1;

    // Initialize TSS
    zeromem(&data->tss, sizeof(TaskStateSegment));
    data->tss.rsp0 = reinterpret_cast<uint64_t>(kernelStack);
    data->tss.ioMapBase = sizeof(TaskStateSegment);

    // Initialize TSS descriptor
    setTSSDescriptorBase(&data->tssDescriptor, (uint64_t)&data->tss);
    setTSSDescriptorLimit(&data->tssDescriptor, sizeof(TaskStateSegment) - 1);
    data->tssDescriptor.accessByte.type = 0x9;  // 0b1001 for 64-bit TSS (Available)
    data->tssDescriptor.accessByte.present = 1;
    data->tssDescriptor.accessByte.dpl = 0; // Kernel privilege level
    data->tssDescriptor.accessByte.zero = 0; // Should be zero
    data->tssDescriptor.limitHigh = 0; // 64-bit TSS doesn't use limitHigh, set it to 0
    data->tssDescriptor.available = 1; // If you use this field, set it to 1
    data->tssDescriptor.granularity = 0; // No granularity for TSS
    data->tssDescriptor.zero = 0; // Should be zero
    data->tssDescriptor.zeroAgain = 0; // Should be zero

    // Update the GDT with initialized descriptors
    data->gdt.kernelNull = data->kernelNullDescriptor;
    data->gdt.kernelCode = data->kernelCodeDescriptor;
    data->gdt.kernelData = data->kernelDataDescriptor;
    data->gdt.userCode = data->userCodeDescriptor;
    data->gdt.userData = data->userDataDescriptor;
    data->gdt.tss = data->tssDescriptor;

    // Initialize the GDT descriptor
    data->gdtDescriptor = {
        .limit = sizeof(GDT) - 1,
        .base = (uint64_t)&data->gdt
    };

    // Install the GDT
    __kinstall_gdt_asm(&data->gdtDescriptor);

    // Load the Task Register (TR)
    __asm__("ltr %%ax" : : "a" (__TSS_PT1_SELECTOR));

    __per_cpu_data.__cpu[apicid].defaultKernelStack = reinterpret_cast<uint64_t>(kernelStack);

    // Store the address of the tss in both gsbase and k_gsbase
    writeMsr(IA32_GS_BASE, (uint64_t)&__per_cpu_data.__cpu[apicid]);
}
