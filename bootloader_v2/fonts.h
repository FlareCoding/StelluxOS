#ifndef FONTS_H
#define FONTS_H
#include <efi.h>
#include <efilib.h>

typedef struct psf1_hdr {
    unsigned char Magic[2];
    unsigned char Mode;
    unsigned char CharSize;
} psf1_hdr_t;

typedef struct psf1_font {
    psf1_hdr_t* Header;
    void* GlyphBuffer;
} psf1_font_t;

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

psf1_font_t*
LoadPSF1Font(
    EFI_FILE* Dir,
    CHAR16* Path,
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE* SystemTable
);

#endif
