/* settings.c on the build machine: each line setting's termios2 bits, the fields left alone, and the description */
#define _POSIX_C_SOURCE 200809L

#include "settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_REPORTS     8
#define DESCRIPTION_MAX 64
#define ALL_FLAGS       0xFFFFFFFFu
#define FILL_BYTE       0x5A

// The c_cflag bits every apply writes, whatever they held
static const uint32_t WRITTEN_FLAGS = STLX_CBAUD | STLX_CIBAUD | STLX_CSIZE | STLX_CSTOPB | STLX_CREAD | STLX_PARENB |
                                      STLX_PARODD | STLX_CMSPAR | STLX_CRTSCTS;

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

/* Settings another program left, every byte FILL_BYTE but c_cflag */
static struct stlx_termios2 left_with(uint32_t cflag) {
    struct stlx_termios2 t;
    memset(&t, FILL_BYTE, sizeof(t));
    t.c_cflag = cflag;

    return t;
}

static uint32_t applied_flags(const line_settings* settings, uint32_t cflag) {
    struct stlx_termios2 t = left_with(cflag);
    line_settings_apply(settings, &t);

    return t.c_cflag;
}

static uint32_t parity_flags(parity_kind parity) {
    line_settings settings = line_settings_default();
    settings.parity = parity;

    return applied_flags(&settings, ALL_FLAGS) & (STLX_PARENB | STLX_PARODD | STLX_CMSPAR);
}

static bool describes(const line_settings* settings, const char* expected) {
    char text[DESCRIPTION_MAX];
    line_settings_describe(settings, text, sizeof(text));

    return strcmp(text, expected) == 0;
}

static void check_defaults(void) {
    line_settings settings = line_settings_default();
    check(settings.baud == 115200 && settings.data_bits == 8 && settings.parity == PARITY_NONE &&
              settings.stop_bits == 1 && settings.flow == FLOW_NONE && settings.hupcl == HUPCL_KEEP,
          "the default is 115200 baud 8N1 with no flow control, HUPCL kept");

    check(applied_flags(&settings, 0) == (STLX_BOTHER | STLX_CREAD | STLX_CS8),
          "the default on cleared flags sets BOTHER, CREAD and CS8 alone");
    check(applied_flags(&settings, ALL_FLAGS) == ((ALL_FLAGS & ~WRITTEN_FLAGS) | STLX_BOTHER | STLX_CREAD | STLX_CS8),
          "the default writes every decided bit and keeps every other one, HUPCL and CLOCAL included");
}

static void check_framing(void) {
    line_settings settings = line_settings_default();
    const uint32_t sizes[] = { [5] = STLX_CS5, [6] = STLX_CS6, [7] = STLX_CS7, [8] = STLX_CS8 };
    bool is_each_size_right = true;
    for (uint8_t bits = 5; bits <= 8; bits++) {
        settings.data_bits = bits;
        is_each_size_right = is_each_size_right && (applied_flags(&settings, ALL_FLAGS) & STLX_CSIZE) == sizes[bits];
    }

    check(is_each_size_right, "each data bit count sets its CSIZE");

    check(parity_flags(PARITY_NONE) == 0, "no parity clears PARENB, PARODD and CMSPAR");
    check(parity_flags(PARITY_EVEN) == STLX_PARENB, "even parity is PARENB alone");
    check(parity_flags(PARITY_ODD) == (STLX_PARENB | STLX_PARODD), "odd parity is PARENB and PARODD");
    check(parity_flags(PARITY_MARK) == (STLX_PARENB | STLX_PARODD | STLX_CMSPAR), "mark parity sets all three");
    check(parity_flags(PARITY_SPACE) == (STLX_PARENB | STLX_CMSPAR), "space parity is PARENB and CMSPAR");

    settings = line_settings_default();
    settings.parity = PARITY_MARK;
    uint32_t after_mark = applied_flags(&settings, 0);
    settings.parity = PARITY_EVEN;
    check((applied_flags(&settings, after_mark) & (STLX_PARODD | STLX_CMSPAR)) == 0, "even after mark clears CMSPAR");

    settings = line_settings_default();
    settings.stop_bits = 2;
    check(applied_flags(&settings, 0) & STLX_CSTOPB, "2 stop bits set CSTOPB");
    settings.stop_bits = 1;
    check(!(applied_flags(&settings, ALL_FLAGS) & STLX_CSTOPB), "1 stop bit clears CSTOPB");

    settings.flow = FLOW_RTSCTS;
    check(applied_flags(&settings, 0) & STLX_CRTSCTS, "RTS/CTS sets CRTSCTS");
    settings.flow = FLOW_NONE;
    check(!(applied_flags(&settings, ALL_FLAGS) & STLX_CRTSCTS), "no flow control clears CRTSCTS");
}

static void check_hupcl(void) {
    line_settings settings = line_settings_default();
    check(applied_flags(&settings, STLX_HUPCL) & STLX_HUPCL, "a kept HUPCL stays set");
    check(!(applied_flags(&settings, 0) & STLX_HUPCL), "a kept HUPCL stays clear");

    settings.hupcl = HUPCL_ON;
    check(applied_flags(&settings, 0) & STLX_HUPCL, "--hupcl on sets HUPCL");
    settings.hupcl = HUPCL_OFF;
    check(!(applied_flags(&settings, ALL_FLAGS) & STLX_HUPCL), "--hupcl off clears HUPCL");
}

static void check_other_fields(void) {
    line_settings settings = line_settings_default();
    settings.baud = 250000;
    struct stlx_termios2 left = left_with(ALL_FLAGS);
    struct stlx_termios2 t = left;
    line_settings_apply(&settings, &t);

    check((t.c_cflag & STLX_CBAUD) == STLX_BOTHER && (t.c_cflag & STLX_CIBAUD) == 0,
          "the speed goes as BOTHER, with CIBAUD cleared so the input speed follows");
    check(t.c_ispeed == 250000 && t.c_ospeed == 250000, "both speeds are the baud");
    check(t.c_iflag == left.c_iflag && t.c_oflag == left.c_oflag && t.c_lflag == left.c_lflag &&
              t.c_line == left.c_line,
          "c_iflag, c_oflag, c_lflag and c_line stay as they were");

    check(t.c_cc[STLX_VMIN] == 1 && t.c_cc[STLX_VTIME] == 0, "VMIN is 1 and VTIME 0");
    bool are_others_kept = true;
    for (size_t i = 0; i < STLX_TERMIOS2_NCCS; i++) {
        if (i != STLX_VMIN && i != STLX_VTIME) {
            are_others_kept = are_others_kept && t.c_cc[i] == left.c_cc[i];
        }
    }

    check(are_others_kept, "every other control character stays as it was");
}

static void check_descriptions(void) {
    line_settings settings = line_settings_default();
    check(describes(&settings, "115200 baud"), "8N1 with no flow control names the speed alone");

    settings.flow = FLOW_RTSCTS;
    check(describes(&settings, "115200 baud RTS/CTS"), "RTS/CTS follows the speed");

    settings = (line_settings){ .baud = 250000, .data_bits = 7, .parity = PARITY_MARK, .stop_bits = 2,
                                .flow = FLOW_RTSCTS };
    check(describes(&settings, "250000 baud 7M2 RTS/CTS"), "another frame is named after the speed");

    settings = line_settings_default();
    settings.parity = PARITY_SPACE;
    check(describes(&settings, "115200 baud 8S1"), "space parity is S");
    settings.parity = PARITY_EVEN;
    settings.stop_bits = 2;
    check(describes(&settings, "115200 baud 8E2"), "even parity is E");

    settings = line_settings_default();
    settings.stop_bits = 2;
    check(describes(&settings, "115200 baud 8N2"), "2 stop bits alone are named");
    settings = line_settings_default();
    settings.data_bits = 7;
    check(describes(&settings, "115200 baud 7N1"), "7 data bits alone are named");
}

int main(void) {
    check_defaults();
    check_framing();
    check_hupcl();
    check_other_fields();
    check_descriptions();

    if (g_failures != 0) {
        printf("settings: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("settings: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
