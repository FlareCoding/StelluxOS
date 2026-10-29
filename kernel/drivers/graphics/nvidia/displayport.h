// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAYPORT_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAYPORT_H

#include "common/types.h"
#include "drivers/graphics/edid.h"

/*
 * A DisplayPort link sends a monitor its picture over 1, 2 or 4 lanes, the
 * wire pairs that each carry a share of the data. Each lane sends symbols
 * (each a byte of pixels, other data or framing) at the link's rate in
 * symbols per second. The card's connector and the monitor each take at most
 * some number of lanes and at most some rate, the monitor reporting its
 * limits in its DPCD, the registers a DisplayPort monitor exposes over the
 * cable's auxiliary channel. Link training is the exchange in which the
 * monitor recovers each lane's clock, equalizes its signal, locks onto its
 * symbols and aligns the lanes with each other, at one configuration (a lane
 * count and a rate) within both ends' limits. The link then carries a single
 * stream of one timing's pixels.
 *
 * A timing's horizontal blanking is the part of each line outside its visible
 * pixels (its porches and sync pulse), and its vertical blanking is the lines
 * outside a frame's visible lines. A configuration carries a timing when its
 * lanes, at their rate less 0.5%, move the timing's pixels faster than the
 * timing sends them, each line's horizontal blanking has room for the symbols
 * the link sends around the line's pixels and 3 pixels more, and the timing
 * is wider than 60 pixels. A monitor may take enhanced framing, which marks
 * where each line's pixels start and end with longer runs of symbols. The
 * stream packs each line's pixels into transfer units, runs of 64 symbols per
 * lane holding pixels and filler, and its parameters tell the firmware how
 * the packing runs. The watermark is the number of symbols each lane's buffer
 * holds before the link starts sending a line's pixels. The blanking symbols
 * are those the blanking leaves for packets (data other than pixels, such as
 * audio).
 *
 * These functions decide, for a link's facts and a timing from the EDID
 * parser, whether a configuration carries the timing and with which
 * parameters, the lowest configuration that carries it, whether the status
 * the monitor reports after training shows the link trained, and whether the
 * monitor is in sync with the stream. They keep no state, touch no hardware
 * and never block.
 *
 * They follow the arithmetic and order of NVIDIA's DisplayPort library for a
 * GPU whose links support forward error correction (FEC, check data that lets
 * the monitor repair errors), with FEC left off, as NVIDIA's driver leaves it
 * off for every stream it does not compress unless the code calling the
 * library sets a preferred configuration with it, and this driver compresses
 * none and sets none. Every stream sends 24 bits per pixel, 8 for each of
 * red, green and blue.
 *
 * Five of their rules are stricter than NVIDIA's library. A GPU whose links
 * lack FEC support gets no configuration, where NVIDIA's library decides by
 * another rule. The connector's lanes and rate cap the monitor's, where
 * NVIDIA's library takes the monitor's lanes alone and caps the rate by the
 * GPU's own highest, and by the connector's only when the firmware or a
 * registry key asks for it. Limits outside the values `link_limits` takes
 * leave no configuration, where NVIDIA's library maps them onto values it
 * takes or searches within them, and so can still find one. The search
 * refuses a pixel clock of 0, which NVIDIA's takes as carried, and both
 * functions refuse 8 lanes, which NVIDIA's library takes.
 */

namespace drivers::nvidia::displayport {

// A lane's rate as DisplayPort codes it, in units of 27 million symbols per second
enum class link_rate : uint8_t {
    rbr  = 0x06, // 162 million symbols per second
    hbr  = 0x0A, // 270 million
    hbr2 = 0x14, // 540 million
    hbr3 = 0x1E, // 810 million
};

// The most lanes and the highest rate one end of a link takes: 1, 2 or 4 lanes and one of the four rates. Limits
// holding any other value, 0 included, leave no configuration, so a caller must map a DPCD's other values onto these
struct link_limits {
    uint8_t   max_lanes;
    link_rate max_rate;
};

// What decides the configurations a link can run and how a stream fills each
struct link_facts {
    link_limits connector;                  // the card's connector, its rate capped by the GPU's own highest
    link_limits monitor;                    // the monitor, mapped from its DPCD
    bool        enhanced_framing;           // the monitor takes enhanced framing, which the stream must then run
    bool        fec_supported;              // the GPU's links support FEC
    bool        increased_watermark_limits; // the GPU's links take the increased watermark limits
};

// The lane count and rate link training sets both ends of a link to
struct link_configuration {
    uint8_t   lanes; // 1, 2 or 4
    link_rate rate;
};

// How a stream packs a timing's pixels on a configuration, as the firmware's stream configuration takes it
struct stream_parameters {
    uint32_t transfer_unit_symbols; // symbols per lane in each transfer unit, 64
    uint32_t watermark;             // in symbols, at least 20, or 22 under the increased watermark limits
    uint32_t hblank_symbols;        // symbols per lane each line's horizontal blanking leaves for packets
    uint32_t vblank_symbols;        // symbols per lane each line of vertical blanking leaves for packets
};

// A link's status, laid out as the four bytes of the monitor's DPCD from 0x202. A lane's bits 0, 1 and 2 are set once
// its clock is recovered, its equalization done and its symbols locked
struct link_status {
    uint8_t lanes_0_1;   // 0x202, lane 0 in bits 2 to 0 and lane 1 in bits 6 to 4
    uint8_t lanes_2_3;   // 0x203, lane 2 in bits 2 to 0 and lane 3 in bits 6 to 4
    uint8_t lane_align;  // 0x204, bit 0 set once the lanes are aligned with each other
    uint8_t sink_status; // 0x205, bit 0 set while the monitor's first port is in sync with the stream
};

/**
 * @brief Decide whether `config` carries `timing` on a link with `facts`, and
 * the stream's parameters when it does.
 * @param facts The link's facts. Only `enhanced_framing`, `fec_supported` and
 * `increased_watermark_limits` count, since `config` must lie within both
 * ends' limits, as `find_lowest_configuration`'s result does.
 * @param config The configuration.
 * @param timing The timing.
 * @param out Set when `config` carries `timing`, unchanged otherwise.
 * @return Whether `config` carries `timing`. It does not when the GPU's links
 * lack FEC support, when `config` has a lane count other than 1, 2 or 4,
 * when the pixel clock is 0, when its lanes at their rate less 0.5% move no
 * more bits per second than the pixels need, when the horizontal blanking is
 * shorter than the symbols around a line's pixels, at the lanes' full rate,
 * plus 3 pixels, or when the timing is 60 pixels wide or narrower.
 */
bool compute_stream(
    const link_facts& facts,
    const link_configuration& config,
    const edid::timing& timing,
    stream_parameters& out
);

/**
 * @brief Find the lowest configuration within both ends' limits that carries
 * `timing`, as `compute_stream` decides.
 * @param facts The link's facts.
 * @param timing The timing.
 * @param out Set to the configuration when one carries `timing`, unchanged
 * otherwise.
 * @return Whether a configuration carries `timing`. None does when either
 * end's limits hold a value `link_limits` does not take, when the GPU's links
 * lack FEC support, or when the pixel clock is 0. Configurations are tried in
 * NVIDIA's order, by lanes times rate, the fewer lanes first where two give
 * the same product, so the configuration found sends the fewest symbols per
 * second.
 */
bool find_lowest_configuration(const link_facts& facts, const edid::timing& timing, link_configuration& out);

/**
 * @brief Decide whether `status` shows each of a link's first `lanes` lanes
 * trained and the lanes aligned with each other, as NVIDIA's library judges a
 * link trained. `sink_status` is not read.
 * @param status The status, read from the monitor's DPCD.
 * @param lanes The lane count the link trained at, which must be 1, 2 or 4.
 * @return Whether the link is trained.
 */
bool is_trained(const link_status& status, uint8_t lanes);

/**
 * @brief Decide whether `status` shows the monitor in sync with the stream on
 * its first port.
 * @param status The status, read from the monitor's DPCD.
 * @return Whether the monitor is in sync.
 */
bool is_in_sync(const link_status& status);

} // namespace drivers::nvidia::displayport

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAYPORT_H
