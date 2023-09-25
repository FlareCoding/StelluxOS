#include "kprint.h"
#include "kstring.h"
#include <graphics/kdisplay.h>
#include <ports/serial.h>
#include <stdarg.h>

#define CHAR_PIXEL_WIDTH 8
#define CHAR_TOP_BORDER_OFFSET 8
#define CHAR_LEFT_BORDER_OFFSET 8

Point g_cursorLocation = { .x = CHAR_LEFT_BORDER_OFFSET, .y = CHAR_TOP_BORDER_OFFSET };

void kprintSetCursorLocation(uint32_t x, uint32_t y) {
    g_cursorLocation.x = (x == static_cast<uint32_t>(-1)) ? CHAR_LEFT_BORDER_OFFSET : x;
    g_cursorLocation.y = (y == static_cast<uint32_t>(-1)) ? CHAR_LEFT_BORDER_OFFSET : y;
}

void kprintCharColored(
    char chr,
	unsigned int color
) {
    // Log the character to the serial port in 
    // addition to committing it to VGA memory.
    writeToSerialPort(SERIAL_PORT_BASE_COM1, chr);

    uint8_t charPixelHeight = Display::getTextFontInfo()->header->charSize;
    
    switch (chr) {
        case '\n': {
            g_cursorLocation.x = CHAR_LEFT_BORDER_OFFSET;
            g_cursorLocation.y += charPixelHeight;
            break;
        }
        case '\r': {
            g_cursorLocation.x = CHAR_LEFT_BORDER_OFFSET;
            break;
        }
        default: {
            Display::renderTextGlyph(chr, g_cursorLocation.x, g_cursorLocation.y, color);
            g_cursorLocation.x += CHAR_PIXEL_WIDTH;

            if (g_cursorLocation.x + CHAR_PIXEL_WIDTH > Display::getFramebuffer().width) {
                g_cursorLocation.x = CHAR_LEFT_BORDER_OFFSET;
                g_cursorLocation.y += charPixelHeight;
            }
            break;
        }
    }
}

void kprintChar(
    char chr
) {
    kprintCharColored(chr, DEFAULT_TEXT_COLOR);
}

void kprintColoredEx(
    const char* str,
    uint32_t color
) {
    char* chr = const_cast<char*>(str);
    while (*chr) {
        kprintCharColored(*chr, color);
        ++chr;
    }
}

void kprintFmtColoredEx(
    uint32_t color,
    const char* fmt,
    va_list args
) {
    bool fmtDiscovered = false;

    while (*fmt)
    {
        // If the '%' character is discovered,
        // treat it as a start of an arg format.
        if (*fmt == '%') {
            fmtDiscovered = true;
            ++fmt;
            continue;
        }

        // If there is no format arg expected,
        // print the character normally.
        if (!fmtDiscovered) {
            kprintCharColored(*fmt, color);
            ++fmt;
            continue;
        }

        switch (*fmt) {
        case 'c': {
            char ch = (char)va_arg(args, int);
            kprintCharColored(ch, color);
            break;
        }
        case 's': {
            char* substr = va_arg(args, char*);
            kprintColoredEx(substr, color);
            break;
        }
        case 'i': {
            int arg = va_arg(args, int);

            // Convert the integer to a string using a local buffer
            char buf[128] = { 0 };
            lltoa(arg, buf, sizeof(buf));

            kprintColoredEx(buf, color);
            break;
        }
        case 'x': {
            uint32_t arg = va_arg(args, uint32_t);

            // Convert the integer to a hex string using a local buffer
            char buf[17] = { 0 };
            htoa(arg, buf, sizeof(buf));

            kprintColoredEx(buf, color);
            break;
        }
        case 'l': {
            char buf[128] = { 0 };

            //
            // Testing multi-character format args:
            //
            // %llu --> unsigned 64bit integer
            if (*(fmt + 1) == 'l' && *(fmt + 2) == 'u') {
                uint64_t arg = va_arg(args, uint64_t);
                lltoa(arg, buf, sizeof(buf));
                
                fmt += 2;
            } 
            // %lli --> signed 64bit integer
            else if (*(fmt + 1) == 'l' && *(fmt + 2) == 'i') {
                int64_t arg = va_arg(args, int64_t);
                lltoa(arg, buf, sizeof(buf));
                
                fmt += 2;
            }
            // %llx --> unsigned 64bit integer converted to hex string
            else if (*(fmt + 1) == 'l' && *(fmt + 2) == 'x') {
                uint64_t arg = va_arg(args, uint64_t);
                htoa(arg, buf, sizeof(buf));
                
                fmt += 2;
            } else {
                int32_t arg = va_arg(args, int32_t);
                lltoa(arg, buf, sizeof(buf));
            }

            kprintColoredEx(buf, color);
            break;
        }
        default: {
            kprintCharColored(*fmt, color);
            break;
        };
        }

        fmtDiscovered = false;
        ++fmt;
    }
}

void kprintFmtColored(
    uint32_t color,
    const char* fmt,
    ...
) {
    va_list args;
    va_start(args, fmt);

    kprintFmtColoredEx(color, fmt, args);

    va_end(args);
}

void kprint(
    const char* fmt,
    ...
) {
    va_list args;
    va_start(args, fmt);

    kprintFmtColoredEx(DEFAULT_TEXT_COLOR, fmt, args);

    va_end(args);
}

void kprintError(
    const char* fmt,
    ...
) {
    // Print the error heading
    kprintColoredEx("#ERROR ", TEXT_COLOR_RED);

    va_list args;
    va_start(args, fmt);

    // Print the actual message
    kprintFmtColoredEx(TEXT_COLOR_WHITE, fmt, args);
    
    va_end(args);
}

void kprintWarn(
    const char* fmt,
    ...
) {
    // Print the warning heading
    kprintColoredEx("#WARN ", TEXT_COLOR_YELLOW);

    va_list args;
    va_start(args, fmt);

    // Print the actual message
    kprintFmtColoredEx(TEXT_COLOR_WHITE, fmt, args);
    
    va_end(args);
}

void kprintInfo(
    const char* fmt,
    ...
) {
    // Print the info heading
    kprintColoredEx("#INFO ", TEXT_COLOR_GREEN);

    va_list args;
    va_start(args, fmt);

    // Print the actual message
    kprintFmtColoredEx(TEXT_COLOR_WHITE, fmt, args);
    
    va_end(args);
}
