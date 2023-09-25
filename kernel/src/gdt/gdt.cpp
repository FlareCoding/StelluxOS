#include "gdt.h"
#include <memory/kmemory.h>

EXTERN_C void __kinstall_gdt_asm(GdtDescriptor* descriptor);

GDT g_globalDescriptorTable;
GdtSegmentDescriptor kernelNullDescriptor;
GdtSegmentDescriptor kernelCodeDescriptor;
GdtSegmentDescriptor kernelDataDescriptor;
GdtSegmentDescriptor userNullDescriptor;
GdtSegmentDescriptor userCodeDescriptor;
GdtSegmentDescriptor userDataDescriptor;

GdtDescriptor g_gdtDescriptor = {
    .limit = sizeof(GDT) - 1,
    .base = (uint64_t)&g_globalDescriptorTable
};

void setSegmentDescriptorBase(
    GdtSegmentDescriptor* descriptor,
    uint64_t base
) {
    descriptor->baseLow  = (base & 0xffff);
    descriptor->baseMid  = (base >> 16) & 0xFF;
    descriptor->baseHigh = (base >> 24) & 0xFF;
}

void setSegmentDescriptorLimit(
    GdtSegmentDescriptor* descriptor,
    uint64_t limit
) {
    // Set the lower 16 bits of the limit
    descriptor->limitLow = (uint16_t)(limit & 0xFFFF);

    // Set the higher 4 bits of the limit
    descriptor->limitHigh = (uint8_t)((limit >> 16) & 0xF);
}

void intializeAndInstallGDT() {
    // Zero out all descriptors initially
    zeromem(&kernelNullDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&kernelCodeDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&kernelDataDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&userNullDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&userCodeDescriptor, sizeof(GdtSegmentDescriptor));
    zeromem(&userDataDescriptor, sizeof(GdtSegmentDescriptor));
    
    // Initialize Kernel Code Segment
    setSegmentDescriptorBase(&kernelCodeDescriptor, 0);
    setSegmentDescriptorLimit(&kernelCodeDescriptor, 0xFFFFF);
    kernelCodeDescriptor.longMode = 1;
    kernelCodeDescriptor.granularity = 1;
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
    userDataDescriptor.accessByte.present = 1;
    userDataDescriptor.accessByte.descriptorPrivilegeLvl = 3; // Usermode privilege level
    userDataDescriptor.accessByte.executable = 0; // Data segment
    userDataDescriptor.accessByte.readWrite = 1;
    userDataDescriptor.accessByte.descriptorType = 1;

    // Update the GDT with initialized descriptors
    g_globalDescriptorTable.kernelNull = kernelNullDescriptor;
    g_globalDescriptorTable.kernelCode = kernelCodeDescriptor;
    g_globalDescriptorTable.kernelData = kernelDataDescriptor;
    g_globalDescriptorTable.userNull = userNullDescriptor;
    g_globalDescriptorTable.userCode = userCodeDescriptor;
    g_globalDescriptorTable.userData = userDataDescriptor;

    // Install the GDT
    __kinstall_gdt_asm(&g_gdtDescriptor);
}
