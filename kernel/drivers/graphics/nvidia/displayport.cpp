// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/displayport.h"

namespace drivers::nvidia::displayport {

// Every configuration of 1, 2 or 4 lanes at the four standard rates, in NVIDIA's order: by lanes times rate, the
// fewer lanes first where two give the same product
static constexpr link_configuration CONFIGURATIONS[] = {
    {1, link_rate::rbr},  {1, link_rate::hbr},  {2, link_rate::rbr},  {1, link_rate::hbr2},
    {2, link_rate::hbr},  {4, link_rate::rbr},  {1, link_rate::hbr3}, {2, link_rate::hbr2},
    {4, link_rate::hbr},  {2, link_rate::hbr3}, {4, link_rate::hbr2}, {4, link_rate::hbr3},
};

static constexpr uint64_t SYMBOLS_PER_SECOND_PER_RATE_UNIT = 27000000;
static constexpr uint32_t BITS_PER_SYMBOL                  = 8;
static constexpr uint32_t BITS_PER_PIXEL                   = 24;
static constexpr uint32_t TRANSFER_UNIT_SYMBOLS            = 64;

// The fixed point NVIDIA's library computes a stream's fill ratio in, `RATIO_ONE` standing for full transfer units
static constexpr uint64_t RATIO_ONE = 100000;

// The watermark's fixed addend and its lowest value, under NVIDIA's normal limits and under its increased limits
static constexpr uint32_t WATERMARK_ADJUST            = 2;
static constexpr uint32_t WATERMARK_MINIMUM           = 20;
static constexpr uint32_t INCREASED_WATERMARK_ADJUST  = 8;
static constexpr uint32_t INCREASED_WATERMARK_MINIMUM = 22;

// The bits NVIDIA's library counts around each line's pixels (a part per link and a part per lane, not counting the
// padding), and the symbols per lane enhanced framing adds to them
static constexpr uint32_t BLANKING_BITS_PER_LINK   = 3 * 8 * 4;
static constexpr uint32_t BLANKING_BITS_PER_LANE   = 2 * 8;
static constexpr uint32_t ENHANCED_FRAMING_SYMBOLS = 3;

static constexpr uint32_t MIN_HBLANK_MARGIN_PIXELS = 3;

// The narrowest timing NVIDIA's library lets a link carry
static constexpr uint32_t MIN_WIDTH = 61;

// The symbols a line's horizontal blanking loses to the display engine's latency, 1 in sending the symbol starting the
// blanking and 3 in starting packets, and the margin NVIDIA's library keeps in either blanking
static constexpr int32_t ENGINE_LATENCY_SYMBOLS = 1 + 3;
static constexpr int32_t MARGIN_SYMBOLS         = 3;

// The most the symbol starting a line's blanking lags the line's pixels (1.5 transfer units), and the main stream
// attributes, the packet telling the monitor the stream's timing and format (36 symbols over the lanes and 3 on each)
static constexpr int32_t  BLANKING_START_LAG_SYMBOLS = TRANSFER_UNIT_SYMBOLS * 3 / 2;
static constexpr uint32_t MSA_LINK_SYMBOLS           = 36;
static constexpr uint32_t MSA_LANE_SYMBOLS           = 3;

static constexpr uint8_t LANE_TRAINED           = 0x07;
static constexpr uint8_t INTERLANE_ALIGN_DONE   = 0x01;
static constexpr uint8_t RECEIVE_PORT_0_IN_SYNC = 0x01;

static uint64_t divide_ceil(uint64_t dividend, uint64_t divisor) {
    return (dividend + divisor - 1) / divisor;
}

static int32_t at_least_zero(int32_t value) {
    return value < 0 ? 0 : value;
}

static bool is_lane_count(uint8_t lanes) {
    return lanes == 1 || lanes == 2 || lanes == 4;
}

static bool is_standard_rate(link_rate rate) {
    switch (rate) {
        case link_rate::rbr:
        case link_rate::hbr:
        case link_rate::hbr2:
        case link_rate::hbr3:
            return true;
    }

    return false;
}

static uint64_t symbols_per_second(link_rate rate) {
    return static_cast<uint8_t>(rate) * SYMBOLS_PER_SECOND_PER_RATE_UNIT;
}

// A lane's rate less 0.5%, the most it loses when the link spreads its clock below the rate to lower interference
static uint64_t lowest_symbols_per_second(link_rate rate) {
    uint64_t peak = symbols_per_second(rate);
    return peak - 5 * peak / 1000;
}

bool compute_stream(
    const link_facts& facts,
    const link_configuration& config,
    const edid::timing& timing,
    stream_parameters& out
) {
    // NVIDIA's library decides for a GPU whose links lack FEC support by another rule, which this file does not follow
    if (!facts.fec_supported) {
        return false;
    }

    // A link runs 1, 2 or 4 lanes, and the lane count and the pixel clock divide below
    if (!is_lane_count(config.lanes) || timing.pixel_clock_khz == 0) {
        return false;
    }

    uint32_t lanes = config.lanes;
    uint64_t pixel_clock_hz = static_cast<uint64_t>(timing.pixel_clock_khz) * 1000;
    uint64_t peak_rate = symbols_per_second(config.rate);
    uint64_t lowest_link_bits_per_second = BITS_PER_SYMBOL * lowest_symbols_per_second(config.rate) * lanes;
    if (pixel_clock_hz * BITS_PER_PIXEL >= lowest_link_bits_per_second) {
        return false;
    }

    // Scaled so `RATIO_ONE` stands for 1: how full the pixels keep each transfer unit, a pixel's symbols per lane and
    // the watermark's term for a part-full transfer unit. The check above keeps the first below `RATIO_ONE`
    uint64_t ratio = pixel_clock_hz * BITS_PER_PIXEL * RATIO_ONE / lowest_link_bits_per_second;
    uint64_t pixel_symbols_per_lane = BITS_PER_PIXEL * RATIO_ONE / (BITS_PER_SYMBOL * lanes);
    uint64_t fill_term = ratio * TRANSFER_UNIT_SYMBOLS * (RATIO_ONE - ratio) / RATIO_ONE;

    uint32_t adjust = facts.increased_watermark_limits ? INCREASED_WATERMARK_ADJUST : WATERMARK_ADJUST;
    uint32_t minimum = facts.increased_watermark_limits ? INCREASED_WATERMARK_MINIMUM : WATERMARK_MINIMUM;

    // The watermark holds the adjustment, 3 pixels' symbols, the fill term and 3 symbols, and adds 8 symbols divided by
    // the lane count at a ratio of 0 and 3 at a ratio of 1, linear between them with the change rounded toward 0
    uint32_t ratio_0_symbols = 8 / lanes;
    int32_t ratio_part = static_cast<int32_t>(ratio) * (3 - static_cast<int32_t>(ratio_0_symbols));
    uint64_t base = adjust + (3 * pixel_symbols_per_lane + fill_term) / RATIO_ONE + ratio_0_symbols + 3;
    uint32_t watermark =
        static_cast<uint32_t>(static_cast<int32_t>(base) + ratio_part / static_cast<int32_t>(RATIO_ONE));
    if (watermark < minimum) {
        watermark = minimum;
    }

    // Each lane's share of a line's pixels is padded to whole pixels
    uint32_t width = timing.width;
    uint32_t width_per_lane = static_cast<uint32_t>(divide_ceil(width, lanes));
    uint64_t line_symbols_per_lane = divide_ceil(width_per_lane * BITS_PER_PIXEL, BITS_PER_SYMBOL);
    uint32_t padded_bits = static_cast<uint32_t>(line_symbols_per_lane) * BITS_PER_SYMBOL * lanes;

    uint32_t blanking_bits = BLANKING_BITS_PER_LINK + BLANKING_BITS_PER_LANE * lanes + padded_bits -
                             width * BITS_PER_PIXEL;
    uint32_t blanking_symbols_per_lane = static_cast<uint32_t>(divide_ceil(blanking_bits, BITS_PER_SYMBOL * lanes));
    if (facts.enhanced_framing) {
        blanking_symbols_per_lane += ENHANCED_FRAMING_SYMBOLS;
    }

    uint64_t blanking_pixels = divide_ceil(blanking_symbols_per_lane * pixel_clock_hz, peak_rate);
    uint32_t min_hblank = static_cast<uint32_t>(blanking_pixels) + MIN_HBLANK_MARGIN_PIXELS;
    uint32_t hblank = timing.h_total() - width;
    if (min_hblank > hblank || width < MIN_WIDTH) {
        return false;
    }

    uint64_t spare_hblank_symbols = static_cast<uint64_t>(hblank - min_hblank) * peak_rate / pixel_clock_hz;
    int32_t hblank_symbols = static_cast<int32_t>(spare_hblank_symbols) - ENGINE_LATENCY_SYMBOLS - MARGIN_SYMBOLS;

    // A line of vertical blanking leaves the symbols of the width less 3 pixels, less the blanking start's lag beyond
    // the horizontal blanking symbols before their clamp at 0, and less the main stream attributes and the margin
    uint64_t active_symbols = static_cast<uint64_t>(width - 3) * peak_rate / pixel_clock_hz;
    int32_t lag_symbols = at_least_zero(BLANKING_START_LAG_SYMBOLS - hblank_symbols);
    int32_t msa_symbols = static_cast<int32_t>(MSA_LINK_SYMBOLS / lanes + MSA_LANE_SYMBOLS);
    int32_t vblank_symbols = static_cast<int32_t>(active_symbols) - lag_symbols - msa_symbols - MARGIN_SYMBOLS;

    out.transfer_unit_symbols = TRANSFER_UNIT_SYMBOLS;
    out.watermark = watermark;
    out.hblank_symbols = static_cast<uint32_t>(at_least_zero(hblank_symbols));
    out.vblank_symbols = static_cast<uint32_t>(at_least_zero(vblank_symbols));

    return true;
}

bool find_lowest_configuration(const link_facts& facts, const edid::timing& timing, link_configuration& out) {
    bool connector_valid = is_lane_count(facts.connector.max_lanes) && is_standard_rate(facts.connector.max_rate);
    bool monitor_valid = is_lane_count(facts.monitor.max_lanes) && is_standard_rate(facts.monitor.max_rate);
    if (!connector_valid || !monitor_valid) {
        return false;
    }

    uint8_t max_lanes = facts.connector.max_lanes < facts.monitor.max_lanes ? facts.connector.max_lanes
                                                                            : facts.monitor.max_lanes;
    link_rate max_rate = facts.connector.max_rate < facts.monitor.max_rate ? facts.connector.max_rate
                                                                           : facts.monitor.max_rate;

    for (const link_configuration& config : CONFIGURATIONS) {
        bool within_limits = config.lanes <= max_lanes && config.rate <= max_rate;
        stream_parameters stream;
        if (within_limits && compute_stream(facts, config, timing, stream)) {
            out = config;
            return true;
        }
    }

    return false;
}

bool is_trained(const link_status& status, uint8_t lanes) {
    for (uint8_t lane = 0; lane < lanes; lane++) {
        uint8_t pair = lane < 2 ? status.lanes_0_1 : status.lanes_2_3;
        uint8_t bits = static_cast<uint8_t>((pair >> (4 * (lane % 2))) & LANE_TRAINED);
        if (bits != LANE_TRAINED) {
            return false;
        }
    }

    return (status.lane_align & INTERLANE_ALIGN_DONE) != 0;
}

bool is_in_sync(const link_status& status) {
    return (status.sink_status & RECEIVE_PORT_0_IN_SYNC) != 0;
}

} // namespace drivers::nvidia::displayport
