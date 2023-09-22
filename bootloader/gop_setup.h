#ifndef GOP_H
#define GOP_H
#include "common.h"

EFI_STATUS RetrieveGraphicsOutputProtocol(
    EFI_HANDLE ImageHandle,
    EFI_GRAPHICS_OUTPUT_PROTOCOL** OutputProtocol
);

#endif // GOP_H
