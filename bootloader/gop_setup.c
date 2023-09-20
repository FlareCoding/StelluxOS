#include "gop_setup.h"

EFI_STATUS InitializeGOP(EFI_HANDLE ImageHandle, EFI_GRAPHICS_OUTPUT_PROTOCOL** Gop) {
    EFI_STATUS Status;

    // Locate Graphics Output Protocol
    Status = uefi_call_wrapper(BS->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, 
                               NULL,
                               (VOID**)Gop);
    if (EFI_ERROR(Status)) {
        Print(L"Error locating Graphics Output Protocol: %r\n\r", Status);
        return Status;
    }

    // For Debugging, print some information about the framebuffer
    Print(L"Framebuffer Base: 0x%lx\n\r", (*Gop)->Mode->FrameBufferBase);
    Print(L"Framebuffer Size: 0x%lx\n\r", (*Gop)->Mode->FrameBufferSize);
    Print(L"Resolution: %ux%u\n\r", (*Gop)->Mode->Info->HorizontalResolution, (*Gop)->Mode->Info->VerticalResolution);

    return EFI_SUCCESS;
}
