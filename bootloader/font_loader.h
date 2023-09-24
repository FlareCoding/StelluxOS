#ifndef FONT_LOADER_H
#define FONT_LOADER_H
#include "file_loader.h"

struct PSF1_Hdr {
    unsigned char Magic[2];
    unsigned char Mode;
    unsigned char CharSize;
};

struct PSF1_Font {
    struct PSF1_Hdr* Header;
    void* GlyphBuffer;
};

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

struct PSF1_Font*
LoadPSF1Font(
    CHAR16* Path,
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE* SystemTable
);

#endif
