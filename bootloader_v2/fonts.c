#include "fonts.h"
#include "loader.h"

psf1_font_t*
LoadPSF1Font(
    EFI_FILE* Dir,
    CHAR16* Path,
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE* SystemTable
) {
    EFI_FILE* font = LoadFile(Dir, Path, ImageHandle, SystemTable);
    if (font == NULL) {
        Print(L"Failed to load PSF1 font\n\r");
        return NULL;
    }

    psf1_hdr_t* FontHeader = AllocateZeroPool(sizeof(psf1_hdr_t));
    UINTN size = sizeof(psf1_hdr_t);
    uefi_call_wrapper(font->Read, 3, font, &size, FontHeader);

    if (FontHeader->Magic[0] != PSF1_MAGIC0 ||
        FontHeader->Magic[1] != PSF1_MAGIC1
    ) {
        Print(L"PSF1 font load error: magic bytes unverified\n\r");
        return NULL;
    }

    UINTN GlyphBufferSize = FontHeader->CharSize * 256;
    if (FontHeader->Mode == 1) {
        GlyphBufferSize = FontHeader->CharSize * 512;
    }

    void* GlyphBuffer = NULL;
    uefi_call_wrapper(font->SetPosition, 2, font, sizeof(psf1_hdr_t));

    GlyphBuffer = AllocateZeroPool(GlyphBufferSize);
    uefi_call_wrapper(font->Read, 3, font, &GlyphBufferSize, GlyphBuffer);

    psf1_font_t* result = AllocateZeroPool(sizeof(psf1_font_t));
    result->Header = FontHeader;
    result->GlyphBuffer = GlyphBuffer;

    return result;
}
