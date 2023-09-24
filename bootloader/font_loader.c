#include "font_loader.h"

struct PSF1_Font*
LoadPSF1Font(
    CHAR16* Path,
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE* SystemTable
) {
    EFI_FILE* FontFile = NULL;
    EFI_FILE* RootDir;
    OpenRootDirectory(&RootDir);

    OpenFile(RootDir, Path, &FontFile);
    if (FontFile == NULL) {
        Print(L"Failed to load PSF1 font\n\r");
        return NULL;
    }

    struct PSF1_Hdr* FontHeader = AllocateZeroPool(sizeof(struct PSF1_Hdr));
    UINTN Size = sizeof(struct PSF1_Hdr);
    uefi_call_wrapper(FontFile->Read, 3, FontFile, &Size, FontHeader);

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
    uefi_call_wrapper(FontFile->SetPosition, 2, FontFile, sizeof(struct PSF1_Hdr));

    GlyphBuffer = AllocateZeroPool(GlyphBufferSize);
    uefi_call_wrapper(FontFile->Read, 3, FontFile, &GlyphBufferSize, GlyphBuffer);

    struct PSF1_Font* result = AllocateZeroPool(sizeof(struct PSF1_Font));
    result->Header = FontHeader;
    result->GlyphBuffer = GlyphBuffer;

    return result;
}
