#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/serial/uart/ns16550.h"
#include "drivers/serial/uart/pl011.h"

TEST_SUITE(uart);

using serial::uart::baud_divisor;
using serial::uart::is_within_tolerance;
using serial::uart::ns16550;
using serial::uart::pl011;
using serial::uart::parity;
using serial::uart::settings;
using serial::uart::ERR_UNSUPPORTED;

static settings line_with(uint8_t data_bits, parity parity_mode, uint8_t stop_bits) {
    return settings{.baud = 115200, .data_bits = data_bits, .parity_mode = parity_mode, .stop_bits = stop_bits};
}

// PCs clock COM1's 16550 at 1.8432 MHz
static constexpr uint32_t PC_UART_CLOCK_HZ = 1843200;

TEST(uart, gives_whole_divisors_for_the_pc_uart_clock) {
    EXPECT_EQ(baud_divisor(PC_UART_CLOCK_HZ, 115200, 0), 1u);
    EXPECT_EQ(baud_divisor(PC_UART_CLOCK_HZ, 9600, 0), 12u);
}

// QEMU's virt board clocks its PL011 at 24 MHz and the Raspberry Pi 4 at 48 MHz, and a PL011 divides in 64ths
static constexpr uint32_t VIRT_PL011_CLOCK_HZ = 24000000;
static constexpr uint32_t RPI4_PL011_CLOCK_HZ = 48000000;
static constexpr uint32_t PL011_FRACTION_BITS = 6;

// At 115200 baud, virt's divisor of 13.02 rounds to 13 and 1/64, and the Pi's 26.04 to 26 and 3/64
TEST(uart, gives_pl011_divisors_in_64ths_rounded_to_the_nearest) {
    EXPECT_EQ(baud_divisor(VIRT_PL011_CLOCK_HZ, 115200, PL011_FRACTION_BITS), 13u * 64 + 1);
    EXPECT_EQ(baud_divisor(RPI4_PL011_CLOCK_HZ, 115200, PL011_FRACTION_BITS), 26u * 64 + 3);
}

// 100 MHz in 64ths is 6.4 GHz, past what 32 bits hold
TEST(uart, keeps_a_scaled_clock_past_32_bits) {
    EXPECT_EQ(baud_divisor(100000000, 115200, PL011_FRACTION_BITS), 3472u);
}

// At 1 baud, a 4 GHz clock needs 16 billion 64ths, past what 32 bits hold
TEST(uart, keeps_a_divisor_past_32_bits) {
    EXPECT_EQ(baud_divisor(4000000000u, 1, PL011_FRACTION_BITS), 16000000000ull);
}

// With a divisor of 1, 1648 Hz makes 103 baud, exactly 3 percent past 100, and 1664 Hz makes 104
TEST(uart, accepts_a_speed_within_3_percent_and_refuses_one_past_it) {
    EXPECT_TRUE(is_within_tolerance(1648, 100, 1));
    EXPECT_FALSE(is_within_tolerance(1664, 100, 1));
}

// 230400 baud rounds to a divisor of 1, which makes half that, and 300000 rounds to 0
TEST(uart, ns16550_refuses_speeds_its_divisors_cannot_make) {
    EXPECT_EQ(ns16550::divisor_for(PC_UART_CLOCK_HZ, 115200), 1);
    EXPECT_EQ(ns16550::divisor_for(PC_UART_CLOCK_HZ, 230400), ERR_UNSUPPORTED);
    EXPECT_EQ(ns16550::divisor_for(PC_UART_CLOCK_HZ, 300000), ERR_UNSUPPORTED);
    EXPECT_EQ(ns16550::divisor_for(PC_UART_CLOCK_HZ, 0), ERR_UNSUPPORTED);
}

// Clocks of 16 times 65535 and 65536 Hz put the divisor for 1 baud at the latch's limit and one past it
TEST(uart, ns16550_divisors_stop_at_the_16_bit_latch) {
    EXPECT_EQ(ns16550::divisor_for(16 * 0xFFFF, 1), 0xFFFF);
    EXPECT_EQ(ns16550::divisor_for(16 * 0x10000, 1), ERR_UNSUPPORTED);
}

// 64 baud from 1024 and 1008 Hz needs 1 and 63/64, and 1 baud from 16 times 65535 Hz needs 65535, one 64th short
// of the divisor 4 baud needs from 4194241 Hz
TEST(uart, pl011_divisors_run_from_1_to_65535) {
    EXPECT_EQ(pl011::divisor_for(1024, 64), 64);
    EXPECT_EQ(pl011::divisor_for(1008, 64), ERR_UNSUPPORTED);
    EXPECT_EQ(pl011::divisor_for(16 * 0xFFFF, 1), 0xFFFF << 6);
    EXPECT_EQ(pl011::divisor_for(4194241, 4), ERR_UNSUPPORTED);
}

TEST(uart, ns16550_codes_each_format_in_its_line_control_bits) {
    EXPECT_EQ(ns16550::line_control_for(line_with(5, parity::none, 1)), 0x00);
    EXPECT_EQ(ns16550::line_control_for(line_with(6, parity::none, 1)), 0x01);
    EXPECT_EQ(ns16550::line_control_for(line_with(7, parity::none, 1)), 0x02);
    EXPECT_EQ(ns16550::line_control_for(line_with(8, parity::none, 1)), 0x03);

    EXPECT_EQ(ns16550::line_control_for(line_with(8, parity::odd, 1)), 0x0B);
    EXPECT_EQ(ns16550::line_control_for(line_with(8, parity::even, 1)), 0x1B);
    EXPECT_EQ(ns16550::line_control_for(line_with(8, parity::none, 2)), 0x07);
    EXPECT_EQ(ns16550::line_control_for(line_with(6, parity::none, 2)), 0x05);
}

// With 5 data bits a 16550 sends one and a half stop bits where two are asked
TEST(uart, ns16550_refuses_formats_it_lacks) {
    EXPECT_EQ(ns16550::line_control_for(line_with(5, parity::none, 2)), ERR_UNSUPPORTED);
    EXPECT_EQ(ns16550::line_control_for(line_with(4, parity::none, 1)), ERR_UNSUPPORTED);
    EXPECT_EQ(ns16550::line_control_for(line_with(9, parity::none, 1)), ERR_UNSUPPORTED);
    EXPECT_EQ(ns16550::line_control_for(line_with(8, parity::none, 0)), ERR_UNSUPPORTED);
    EXPECT_EQ(ns16550::line_control_for(line_with(8, parity::none, 3)), ERR_UNSUPPORTED);
}

TEST(uart, pl011_codes_each_format_in_its_line_control_bits_with_the_fifos_on) {
    EXPECT_EQ(pl011::line_control_for(line_with(5, parity::none, 1)), 0x10);
    EXPECT_EQ(pl011::line_control_for(line_with(6, parity::none, 1)), 0x30);
    EXPECT_EQ(pl011::line_control_for(line_with(7, parity::none, 1)), 0x50);
    EXPECT_EQ(pl011::line_control_for(line_with(8, parity::none, 1)), 0x70);

    EXPECT_EQ(pl011::line_control_for(line_with(8, parity::odd, 1)), 0x72);
    EXPECT_EQ(pl011::line_control_for(line_with(8, parity::even, 1)), 0x76);
    EXPECT_EQ(pl011::line_control_for(line_with(8, parity::none, 2)), 0x78);
    EXPECT_EQ(pl011::line_control_for(line_with(5, parity::none, 2)), 0x18);
}

TEST(uart, pl011_refuses_formats_it_lacks) {
    EXPECT_EQ(pl011::line_control_for(line_with(4, parity::none, 1)), ERR_UNSUPPORTED);
    EXPECT_EQ(pl011::line_control_for(line_with(9, parity::none, 1)), ERR_UNSUPPORTED);
    EXPECT_EQ(pl011::line_control_for(line_with(8, parity::none, 0)), ERR_UNSUPPORTED);
    EXPECT_EQ(pl011::line_control_for(line_with(8, parity::none, 3)), ERR_UNSUPPORTED);
}
