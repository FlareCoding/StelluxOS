#include "hw/rtc.h"
#include "hw/portio.h"
#include "common/time_util.h"
#include "common/logging.h"
#include "acpi/acpi.h"
#include "common/string.h"

namespace rtc {

static uint64_t g_boot_unix_ns;

constexpr uint64_t NS_PER_SEC = 1000000000ULL;

constexpr uint16_t CMOS_ADDR = 0x70;
constexpr uint16_t CMOS_DATA = 0x71;
constexpr uint8_t  NMI_DISABLE = 0x80;

constexpr uint8_t REG_SECONDS  = 0x00;
constexpr uint8_t REG_MINUTES  = 0x02;
constexpr uint8_t REG_HOURS    = 0x04;
constexpr uint8_t REG_DAY      = 0x07;
constexpr uint8_t REG_MONTH    = 0x08;
constexpr uint8_t REG_YEAR     = 0x09;
constexpr uint8_t REG_STATUS_A = 0x0A;
constexpr uint8_t REG_STATUS_B = 0x0B;

constexpr uint8_t STATUS_A_UIP     = 0x80;
constexpr uint8_t STATUS_B_24HR    = 0x02;
constexpr uint8_t STATUS_B_BINARY  = 0x04;

constexpr size_t FADT_CENTURY_OFFSET = 108;

/** @note Privilege: **required** */
__PRIVILEGED_CODE static uint8_t cmos_read(uint8_t reg) {
    portio::out8(CMOS_ADDR, reg | NMI_DISABLE);
    return portio::in8(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t val) {
    return static_cast<uint8_t>((val & 0x0F) + (val >> 4) * 10);
}

/**
 * Read the FADT century register index (ACPI 2.0+, offset 108).
 * Returns 0 if FADT is unavailable or field is zero.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint8_t get_fadt_century_register() {
    const auto* fadt = acpi::find_table("FACP");
    if (!fadt) {
        return 0;
    }

    uint32_t length;
    string::memcpy(&length, &fadt->length, sizeof(length));
    if (length < FADT_CENTURY_OFFSET + sizeof(uint8_t)) {
        return 0;
    }

    uint8_t century_reg;
    string::memcpy(&century_reg,
                   reinterpret_cast<const uint8_t*>(fadt) + FADT_CENTURY_OFFSET,
                   sizeof(century_reg));
    return century_reg;
}

struct rtc_time {
    uint8_t sec, min, hr, day, mon, year, century;
};

/**
 * Read all RTC time registers in one pass.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void read_rtc_raw(rtc_time& t, uint8_t century_reg) {
    t.sec  = cmos_read(REG_SECONDS);
    t.min  = cmos_read(REG_MINUTES);
    t.hr   = cmos_read(REG_HOURS);
    t.day  = cmos_read(REG_DAY);
    t.mon  = cmos_read(REG_MONTH);
    t.year = cmos_read(REG_YEAR);
    t.century = century_reg ? cmos_read(century_reg) : 0;
}

__PRIVILEGED_CODE int32_t init() {
    uint8_t century_reg = get_fadt_century_register();

    // Double-read for consistency: two consecutive reads must match to ensure
    // we didn't read across an RTC update cycle (~244 us window).
    rtc_time t1, t2;
    while (cmos_read(REG_STATUS_A) & STATUS_A_UIP) {}
    read_rtc_raw(t1, century_reg);
    for (;;) {
        while (cmos_read(REG_STATUS_A) & STATUS_A_UIP) {}
        read_rtc_raw(t2, century_reg);
        if (t1.sec == t2.sec && t1.min == t2.min && t1.hr == t2.hr &&
            t1.day == t2.day && t1.mon == t2.mon && t1.year == t2.year &&
            t1.century == t2.century)
            break;
        t1 = t2;
    }

    uint8_t status_b = cmos_read(REG_STATUS_B);
    bool is_binary = (status_b & STATUS_B_BINARY) != 0;
    bool is_24hr   = (status_b & STATUS_B_24HR) != 0;

    uint8_t sec  = t2.sec;
    uint8_t min  = t2.min;
    uint8_t hr   = t2.hr;
    uint8_t day  = t2.day;
    uint8_t mon  = t2.mon;
    uint8_t year = t2.year;
    uint8_t cent = t2.century;

    if (!is_binary) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hr   = bcd_to_bin(static_cast<uint8_t>(hr & 0x7F));
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        if (cent) {
            cent = bcd_to_bin(cent);
        }
    } else {
        hr = static_cast<uint8_t>(hr & 0x7F);
    }

    if (!is_24hr) {
        bool pm = (t2.hr & 0x80) != 0;
        hr = static_cast<uint8_t>(hr % 12);
        if (pm) {
            hr = static_cast<uint8_t>(hr + 12);
        }
    }

    uint32_t full_year;
    if (cent) {
        full_year = static_cast<uint32_t>(cent) * 100 + year;
    } else {
        full_year = (year < 70) ? 2000u + year : 1900u + year;
    }

    uint64_t epoch_sec = time_util::date_to_unix(full_year, mon, day, hr, min, sec);
    g_boot_unix_ns = epoch_sec * NS_PER_SEC;

    log::info("rtc: %u-%02u-%02u %02u:%02u:%02u UTC (epoch=%lu)",
              full_year, static_cast<uint32_t>(mon), static_cast<uint32_t>(day),
              static_cast<uint32_t>(hr), static_cast<uint32_t>(min),
              static_cast<uint32_t>(sec), epoch_sec);

    return OK;
}

uint64_t boot_unix_ns() {
    return g_boot_unix_ns;
}

} // namespace rtc
