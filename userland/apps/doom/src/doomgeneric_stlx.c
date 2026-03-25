/*
 * doomgeneric_stlx.c - Stellux OS platform implementation for doomgeneric
 *
 * Implements the doomgeneric porting interface using the stlxgfx windowing
 * system. DOOM runs as a windowed client application managed by stlxdm.
 *
 * Copyright (C) 2024 Stellux Contributors
 * Based on doomgeneric by ozkl (https://github.com/ozkl/doomgeneric)
 */

#define _POSIX_C_SOURCE 199309L

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

#include <stlxgfx/window.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/event.h>
#include <stlx/input.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- Window state --- */

static stlxgfx_window_t *s_window = NULL;
static struct timespec s_start_time;

/* --- Key queue (ring buffer for DG_GetKey) --- */

#define KEYQUEUE_SIZE 32

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

/* --- HID usage code to DOOM key translation --- */

static unsigned char convertHidToDoomKey(uint16_t usage)
{
    /* Letters a-z: HID 0x04-0x1D → ASCII 'a'-'z' */
    if (usage >= 0x04 && usage <= 0x1D) {
        return (unsigned char)('a' + (usage - 0x04));
    }

    /* Digits 1-9: HID 0x1E-0x26 → ASCII '1'-'9' */
    if (usage >= 0x1E && usage <= 0x26) {
        return (unsigned char)('1' + (usage - 0x1E));
    }

    /* Digit 0: HID 0x27 → ASCII '0' */
    if (usage == 0x27) {
        return '0';
    }

    /* F1-F12: HID 0x3A-0x45 → DOOM KEY_F1-KEY_F12 */
    if (usage >= 0x3A && usage <= 0x45) {
        /* KEY_F1 = 0x80+0x3b, KEY_F2 = 0x80+0x3c, ... */
        static const unsigned char f_keys[] = {
            0x80 + 0x3b, /* F1  */
            0x80 + 0x3c, /* F2  */
            0x80 + 0x3d, /* F3  */
            0x80 + 0x3e, /* F4  */
            0x80 + 0x3f, /* F5  */
            0x80 + 0x40, /* F6  */
            0x80 + 0x41, /* F7  */
            0x80 + 0x42, /* F8  */
            0x80 + 0x43, /* F9  */
            0x80 + 0x44, /* F10 */
            0x80 + 0x57, /* F11 */
            0x80 + 0x58, /* F12 */
        };
        return f_keys[usage - 0x3A];
    }

    switch (usage) {
    case 0x28: return KEY_ENTER;
    case 0x29: return KEY_ESCAPE;
    case 0x2A: return KEY_BACKSPACE;
    case 0x2B: return KEY_TAB;
    case 0x2C: return KEY_USE;          /* Space → Use/Open */
    case 0x2D: return KEY_MINUS;        /* - */
    case 0x2E: return KEY_EQUALS;       /* = */

    /* Punctuation that maps to ASCII */
    case 0x2F: return '[';
    case 0x30: return ']';
    case 0x31: return '\\';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x35: return '`';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';

    /* Arrow keys */
    case 0x4F: return KEY_RIGHTARROW;
    case 0x50: return KEY_LEFTARROW;
    case 0x51: return KEY_DOWNARROW;
    case 0x52: return KEY_UPARROW;

    /* Modifier keys */
    case 0xE0: return KEY_FIRE;         /* Left Ctrl → Fire */
    case 0xE4: return KEY_FIRE;         /* Right Ctrl → Fire */
    case 0xE1: return KEY_RSHIFT;       /* Left Shift → Run */
    case 0xE5: return KEY_RSHIFT;       /* Right Shift → Run */
    case 0xE2: return KEY_LALT;         /* Left Alt → Strafe */
    case 0xE6: return KEY_LALT;         /* Right Alt → Strafe */

    default:
        return 0;
    }
}

static void addKeyToQueue(int pressed, unsigned char key)
{
    if (key == 0) {
        return;
    }

    unsigned short keyData = (unsigned short)((pressed << 8) | key);

    s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
    s_KeyQueueWriteIndex++;
    s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

/* --- doomgeneric platform interface --- */

void DG_Init(void)
{
    memset(s_KeyQueue, 0, sizeof(s_KeyQueue));

    s_window = stlxgfx_create_window(DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                                      "DOOM");
    if (!s_window) {
        printf("doom: failed to create window\n");
        exit(1);
    }

    clock_gettime(CLOCK_MONOTONIC, &s_start_time);

    printf("doom: initialized %dx%d window\n",
           DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_DrawFrame(void)
{
    if (!s_window || !stlxgfx_window_is_open(s_window)) {
        return;
    }

    /* Poll window events and translate to DOOM key events */
    stlxgfx_event_t evt;
    while (stlxgfx_window_next_event(s_window, &evt) == 1) {
        if (evt.type == STLXGFX_EVT_CLOSE_REQUESTED) {
            /* Window closed by the display manager */
            exit(0);
        }

        if (evt.type == STLXGFX_EVT_KEY_DOWN) {
            unsigned char dkey = convertHidToDoomKey(evt.key.usage);
            addKeyToQueue(1, dkey);
        } else if (evt.type == STLXGFX_EVT_KEY_UP) {
            unsigned char dkey = convertHidToDoomKey(evt.key.usage);
            addKeyToQueue(0, dkey);
        }
    }

    /* Copy DG_ScreenBuffer to the window back buffer.
     *
     * DG_ScreenBuffer is filled by I_FinishUpdate() in i_video.c using
     * cmap_to_fb() which outputs pixels in the format configured by
     * I_InitGraphics(). The default mode is "rgba8888":
     *   - red offset   = 16
     *   - green offset = 8
     *   - blue offset  = 0
     *   - alpha offset = 24
     * So each uint32_t pixel = 0xAARRGGBB.
     *
     * The Stellux surface uses the framebuffer's pixel format, which is
     * also typically BGRA (blue_shift=0, green_shift=8, red_shift=16).
     * In both cases the memory layout per pixel is [B, G, R, A] on
     * little-endian, so we can do a straight memcpy.
     */
    stlxgfx_surface_t *buf = stlxgfx_window_back_buffer(s_window);
    if (!buf || !buf->pixels || !DG_ScreenBuffer) {
        return;
    }

    /* Fast path: if pitch matches, single memcpy */
    uint32_t expected_pitch = DOOMGENERIC_RESX * (buf->bpp / 8);
    if (buf->pitch == expected_pitch) {
        memcpy(buf->pixels, DG_ScreenBuffer,
               (size_t)DOOMGENERIC_RESY * expected_pitch);
    } else {
        /* Row-by-row copy if pitches differ */
        uint32_t row_bytes = DOOMGENERIC_RESX * (buf->bpp / 8);
        for (int y = 0; y < DOOMGENERIC_RESY; y++) {
            memcpy(buf->pixels + (uint32_t)y * buf->pitch,
                   (uint8_t *)DG_ScreenBuffer + (uint32_t)y * row_bytes,
                   row_bytes);
        }
    }

    stlxgfx_window_swap_buffers(s_window);
}

void DG_SleepMs(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    uint64_t start_ms = (uint64_t)s_start_time.tv_sec * 1000ULL +
                        (uint64_t)s_start_time.tv_nsec / 1000000ULL;
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000ULL +
                      (uint64_t)now.tv_nsec / 1000000ULL;

    return (uint32_t)(now_ms - start_ms);
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) {
        /* Key queue is empty */
        return 0;
    }

    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex++;
    s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

    *pressed = keyData >> 8;
    *doomKey = keyData & 0xFF;

    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    /* Window title is set at creation; dynamic changes not supported */
    (void)title;
}

/* --- Entry point --- */

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("doom: starting (doomgeneric on Stellux)\n");

    doomgeneric_Create(argc, argv);

    while (1) {
        doomgeneric_Tick();
    }

    return 0;
}
