/*
 * doomgeneric_stlx.c - Stellux platform implementation for doomgeneric
 *
 * DOOM runs as a window protocol client: a fixed-size window, full
 * frame commits paced by buffer availability and the game's own 35 Hz
 * tick, and keyboard input from display manager events.
 *
 * Based on doomgeneric by ozkl (https://github.com/ozkl/doomgeneric)
 */

#define _POSIX_C_SOURCE 199309L

#include "doomkeys.h"
#include "doomgeneric.h"

#include <stlxwin/stlxwin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Window state --- */

static stlxwin_conn *s_conn = NULL;
static stlxwin_window *s_window = NULL;
static struct timespec s_start_time;

/* --- Key queue (ring buffer between event dispatch and DG_GetKey) --- */

#define KEYQUEUE_SIZE 32

static unsigned short s_key_queue[KEYQUEUE_SIZE];
static unsigned int s_key_write = 0;
static unsigned int s_key_read = 0;

/* Keys map from the HID usage alone: releases carry no translated
 * codepoint, and DOOM binds base characters regardless of shift, so
 * usage mapping keeps every down and up pair symmetric. */
static unsigned char translate_key(const stlxwin_event *ev)
{
    uint16_t usage = ev->key.usage;
    if (usage >= 0x04 && usage <= 0x1D) {
        return (unsigned char)('a' + (usage - 0x04));
    }
    if (usage >= 0x1E && usage <= 0x27) {
        static const char digits[] = "1234567890";
        return (unsigned char)digits[usage - 0x1E];
    }
    if (usage >= 0x2D && usage <= 0x38) {
        static const char punct[] = {
            '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/'
        };
        return (unsigned char)punct[usage - 0x2D];
    }
    if (usage == 0x2C) {
        return KEY_USE;
    }
    if (usage >= 0x3A && usage <= 0x45) {
        static const unsigned char f_keys[] = {
            KEY_F1, KEY_F2, KEY_F3, KEY_F4,  KEY_F5,  KEY_F6,
            KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
        };
        return f_keys[usage - 0x3A];
    }

    switch (usage) {
    case 0x28: return KEY_ENTER;
    case 0x29: return KEY_ESCAPE;
    case 0x2A: return KEY_BACKSPACE;
    case 0x2B: return KEY_TAB;
    case 0x4F: return KEY_RIGHTARROW;
    case 0x50: return KEY_LEFTARROW;
    case 0x51: return KEY_DOWNARROW;
    case 0x52: return KEY_UPARROW;

    /* Modifiers are game controls: ctrl fires, shift runs, alt strafes */
    case 0xE0: case 0xE4: return KEY_FIRE;
    case 0xE1: case 0xE5: return KEY_RSHIFT;
    case 0xE2: case 0xE6: return KEY_LALT;

    default:
        return 0;
    }
}

static void queue_key(int pressed, unsigned char key)
{
    if (key == 0) {
        return;
    }

    unsigned int next = (s_key_write + 1) % KEYQUEUE_SIZE;
    if (next == s_key_read) {
        return;
    }

    s_key_queue[s_key_write] = (unsigned short)((pressed << 8) | key);
    s_key_write = next;
}

/* Repeats are dropped: DOOM tracks held keys from down and up pairs */
static void pump_events(void)
{
    stlxwin_dispatch(s_conn);

    stlxwin_event ev;
    while (stlxwin_next_event(s_conn, &ev)) {
        switch (ev.type) {
        case STLXWIN_EVT_CLOSE:
        case STLXWIN_EVT_DISCONNECTED:
            exit(0);
        case STLXWIN_EVT_KEY_DOWN:
            queue_key(1, translate_key(&ev));
            break;
        case STLXWIN_EVT_KEY_UP:
            queue_key(0, translate_key(&ev));
            break;
        default:
            break;
        }
    }
}

/* --- doomgeneric platform interface --- */

void DG_Init(void)
{
    memset(s_key_queue, 0, sizeof(s_key_queue));

    s_conn = stlxwin_connect("doom");
    if (!s_conn) {
        printf("doom: no display manager\r\n");
        exit(1);
    }

    s_window = stlxwin_window_create(s_conn, DOOMGENERIC_RESX,
                                     DOOMGENERIC_RESY, "DOOM", 0);
    if (!s_window) {
        printf("doom: window creation failed\r\n");
        stlxwin_disconnect(s_conn);
        exit(1);
    }

    clock_gettime(CLOCK_MONOTONIC, &s_start_time);

    printf("doom: initialized %dx%d window\r\n",
           DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_DrawFrame(void)
{
    pump_events();

    /* begin_frame blocks until a buffer frees, which is all the frame
     * pacing a 35 Hz game needs. Stride equals width times four by
     * protocol, so the frame is one copy. */
    stlxwin_buffer *buf = stlxwin_begin_frame(s_window);
    if (!buf) {
        exit(0);
    }

    memcpy(buf->pixels, DG_ScreenBuffer,
           (size_t)DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);

    stlxwin_commit(s_window, buf, NULL, 0, 0);
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
    if (s_key_read == s_key_write) {
        return 0;
    }

    unsigned short data = s_key_queue[s_key_read];
    s_key_read = (s_key_read + 1) % KEYQUEUE_SIZE;

    *pressed = data >> 8;
    *doomKey = data & 0xFF;

    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    stlxwin_window_set_title(s_window, title);
}

/* --- Entry point --- */

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    doomgeneric_Create(argc, argv);

    while (1) {
        doomgeneric_Tick();
    }

    return 0;
}
