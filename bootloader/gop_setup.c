#include "gop_setup.h"

EFI_STATUS RetrieveGraphicsOutputProtocol(
    EFI_HANDLE ImageHandle,
    EFI_GRAPHICS_OUTPUT_PROTOCOL** OutputProtocol
) {
    EFI_STATUS Status;

    // Locate Graphics Output Protocol
    Status = uefi_call_wrapper(
        BS->LocateProtocol, 3,
        &gEfiGraphicsOutputProtocolGuid, 
        NULL,
        (VOID**)OutputProtocol
    );

    if (EFI_ERROR(Status)) {
        Print(L"Error locating Graphics Output Protocol: %r\n\r", Status);
        return Status;
    }

    return EFI_SUCCESS;
}
