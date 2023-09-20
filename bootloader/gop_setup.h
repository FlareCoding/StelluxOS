#ifndef GOP_H
#define GOP_H

#include <efi.h>
#include <efilib.h>

EFI_STATUS InitializeGOP(EFI_HANDLE ImageHandle, EFI_GRAPHICS_OUTPUT_PROTOCOL** Gop);

#endif // GOP_H
