#include "drivers/serial/settings.h"

using namespace terminal;

namespace serial {

// The rates of the speed codes B0 to B38400
static constexpr uint32_t BASIC_RATES[] = {
    0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400,
};

// The rates of the speed codes above B38400, which add 1 to 15 to CBAUDEX
static constexpr uint32_t EXTENDED_RATES[] = {
    57600, 115200, 230400, 460800, 500000, 576000, 921600, 1000000,
    1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000,
};

static constexpr uint32_t REFUSED_INPUT_FLAGS = PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IUCLC | IXON | IXOFF;
static constexpr uint32_t REFUSED_LOCAL_FLAGS = ISIG | ICANON | ECHO | ECHONL;

// Every chip makes it, and flashing tools set their own speed
static constexpr uint32_t DEFAULT_SPEED_CODE = B9600;

static bool asks_to_process_bytes(const termios2& settings) {
    return (settings.c_iflag & REFUSED_INPUT_FLAGS) != 0 ||
           (settings.c_oflag & OPOST) != 0 ||
           (settings.c_lflag & REFUSED_LOCAL_FLAGS) != 0;
}

static uint32_t decode_speed(uint32_t code, uint32_t explicit_rate) {
    if (code == BOTHER) {
        return explicit_rate;
    }

    uint32_t index = code & ~CBAUDEX;
    if (code & CBAUDEX) {
        return EXTENDED_RATES[index - 1];
    }

    return BASIC_RATES[index];
}

static uint8_t decode_data_bits(uint32_t cflag) {
    switch (cflag & CSIZE) {
        case CS5: return 5;
        case CS6: return 6;
        case CS7: return 7;
        default:  return 8;
    }
}

static parity decode_parity(uint32_t cflag) {
    if (!(cflag & PARENB)) {
        return parity::none;
    }

    bool is_odd = (cflag & PARODD) != 0;
    if (cflag & CMSPAR) {
        return is_odd ? parity::mark : parity::space;
    }

    return is_odd ? parity::odd : parity::even;
}

termios2 default_termios() {
    termios2 settings = {};
    settings.c_cflag = DEFAULT_SPEED_CODE | CS8 | CREAD | CLOCAL | HUPCL;
    settings.c_cc[VMIN] = 1;
    settings.c_ispeed = decode_speed(DEFAULT_SPEED_CODE, 0);
    settings.c_ospeed = settings.c_ispeed;

    return settings;
}

int32_t translate_termios(const termios2& settings, line_settings* line) {
    if (asks_to_process_bytes(settings)) {
        return ERR_UNSUPPORTED;
    }

    uint32_t output_code = settings.c_cflag & CBAUD;
    uint32_t output_rate = decode_speed(output_code, settings.c_ospeed);
    uint32_t input_rate = decode_speed((settings.c_cflag & CIBAUD) >> IBSHIFT, settings.c_ispeed);
    bool asks_bother_for_zero = output_code == BOTHER && output_rate == 0;
    bool splits_speeds = input_rate != 0 && input_rate != output_rate;
    if (asks_bother_for_zero || splits_speeds) {
        return ERR_UNSUPPORTED;
    }

    line->baud = output_rate;
    line->data_bits = decode_data_bits(settings.c_cflag);
    line->parity_mode = decode_parity(settings.c_cflag);
    line->stop_bits = (settings.c_cflag & CSTOPB) ? 2 : 1;
    line->uses_rts_cts = (settings.c_cflag & CRTSCTS) != 0;

    return OK;
}

} // namespace serial
