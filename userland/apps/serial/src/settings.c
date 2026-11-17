#define _POSIX_C_SOURCE 200809L

#include "settings.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#define DEFAULT_BAUD 115200
#define FRAME_MAX    8

// The c_cflag bits every apply writes, whatever they held before
static const uint32_t WRITTEN_FLAGS = STLX_CBAUD | STLX_CIBAUD | STLX_CSIZE | STLX_CSTOPB | STLX_CREAD | STLX_PARENB |
                                      STLX_PARODD | STLX_CMSPAR | STLX_CRTSCTS;

static const uint32_t SIZE_FLAGS[] = {
    [5] = STLX_CS5,
    [6] = STLX_CS6,
    [7] = STLX_CS7,
    [8] = STLX_CS8,
};

static const uint32_t PARITY_FLAGS[] = {
    [PARITY_NONE]  = 0,
    [PARITY_EVEN]  = STLX_PARENB,
    [PARITY_ODD]   = STLX_PARENB | STLX_PARODD,
    [PARITY_MARK]  = STLX_PARENB | STLX_PARODD | STLX_CMSPAR,
    [PARITY_SPACE] = STLX_PARENB | STLX_CMSPAR,
};

static const char PARITY_LETTERS[] = {
    [PARITY_NONE]  = 'N',
    [PARITY_EVEN]  = 'E',
    [PARITY_ODD]   = 'O',
    [PARITY_MARK]  = 'M',
    [PARITY_SPACE] = 'S',
};

line_settings line_settings_default(void) {
    return (line_settings){
        .baud      = DEFAULT_BAUD,
        .data_bits = 8,
        .parity    = PARITY_NONE,
        .stop_bits = 1,
        .flow      = FLOW_NONE,
        .hupcl     = HUPCL_KEEP,
    };
}

void line_settings_apply(const line_settings* settings, struct stlx_termios2* t) {
    uint32_t flags = (t->c_cflag & ~WRITTEN_FLAGS) | STLX_BOTHER | STLX_CREAD;
    flags |= SIZE_FLAGS[settings->data_bits] | PARITY_FLAGS[settings->parity];
    if (settings->stop_bits == 2) {
        flags |= STLX_CSTOPB;
    }

    if (settings->flow == FLOW_RTSCTS) {
        flags |= STLX_CRTSCTS;
    }

    if (settings->hupcl == HUPCL_ON) {
        flags |= STLX_HUPCL;
    } else if (settings->hupcl == HUPCL_OFF) {
        flags &= ~STLX_HUPCL;
    }

    t->c_cflag = flags;
    t->c_ispeed = settings->baud;
    t->c_ospeed = settings->baud;
    t->c_cc[STLX_VMIN] = 1;
    t->c_cc[STLX_VTIME] = 0;
}

void line_settings_describe(const line_settings* settings, char* text, size_t size) {
    char frame[FRAME_MAX] = "";
    bool is_8n1 = settings->data_bits == 8 && settings->parity == PARITY_NONE && settings->stop_bits == 1;
    if (!is_8n1) {
        snprintf(frame, sizeof(frame), " %u%c%u", (unsigned)settings->data_bits, PARITY_LETTERS[settings->parity],
                 (unsigned)settings->stop_bits);
    }

    const char* flow = settings->flow == FLOW_RTSCTS ? " RTS/CTS" : "";
    snprintf(text, size, "%" PRIu32 " baud%s%s", settings->baud, frame, flow);
}
