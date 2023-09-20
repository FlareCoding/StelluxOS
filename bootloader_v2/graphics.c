#include "graphics.h"

// Global graphics framebuffer instance 
Framebuffer_t g_Framebuffer = {};

Framebuffer_t*
InitGraphicsProtocol(EFI_SYSTEM_TABLE* SystemTable) {
	EFI_GRAPHICS_OUTPUT_PROTOCOL* pGop = NULL;
	EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_STATUS status;
	
	uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3, &gopGuid, NULL, (void**)&pGop);
	g_Framebuffer.Base = (void*)pGop->Mode->FrameBufferBase;
	g_Framebuffer.Size = pGop->Mode->FrameBufferSize;
	g_Framebuffer.Width = pGop->Mode->Info->HorizontalResolution;
	g_Framebuffer.Height = pGop->Mode->Info->VerticalResolution;
	g_Framebuffer.PixelsPerScanline = pGop->Mode->Info->PixelsPerScanLine;

	return &g_Framebuffer;
}
