#ifndef SERIAL_SETTINGS_H
#define SERIAL_SETTINGS_H

/* What a person asks of a port: its line settings, as the termios2 requests carry them, and the levels of DTR and RTS.
 * Ports live in PORT_DIRECTORY under the names the listing gives them */

#include <stlx/termios2.h>

#include <stddef.h>
#include <stdint.h>

#define PORT_DIRECTORY       "/dev/serial"
#define LINE_DESCRIPTION_MAX 64

typedef enum {
    PARITY_NONE,
    PARITY_EVEN,
    PARITY_ODD,
    PARITY_MARK,
    PARITY_SPACE,
} parity_kind;

typedef enum {
    FLOW_NONE,
    FLOW_RTSCTS,
} flow_kind;

typedef enum {
    HUPCL_KEEP,
    HUPCL_ON,
    HUPCL_OFF,
} hupcl_choice;

typedef struct {
    uint32_t     baud;      // above 0
    uint8_t      data_bits; // 5 to 8
    parity_kind  parity;
    uint8_t      stop_bits; // 1 or 2
    flow_kind    flow;
    hupcl_choice hupcl;     // whether the last close lowers DTR and RTS, HUPCL_KEEP leaving it as the port has it
} line_settings;

typedef enum {
    LEVEL_KEEP,
    LEVEL_RAISED,
    LEVEL_LOWERED,
} line_level;

typedef struct {
    line_level dtr;
    line_level rts;
} line_request;

typedef enum {
    PULSE_NONE,
    PULSE_DTR,
    PULSE_RTS,
} line_pulse;

/**
 * @brief 115200 baud, the ESP32 ROM's speed, with 8N1, no flow control and HUPCL kept.
 */
line_settings line_settings_default(void);

/**
 * @brief Writes `settings` into `t`, which holds the port's settings as TCGETS2 read them, and sets CREAD, VMIN 1
 * and VTIME 0, so the port keeps received bytes and `poll` reports the first one. Every other field and c_cflag bit
 * keeps its value, CLOCAL included.
 */
void line_settings_apply(const line_settings* settings, struct stlx_termios2* t);

/**
 * @brief Writes the speed, then the frame unless it is 8N1, then RTS/CTS when on, into `text`, such as "115200 baud"
 * or "250000 baud 7M2 RTS/CTS". LINE_DESCRIPTION_MAX bytes hold any description.
 */
void line_settings_describe(const line_settings* settings, char* text, size_t size);

#endif /* SERIAL_SETTINGS_H */
