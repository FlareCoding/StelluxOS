#ifndef STLXDM_DISPLAY_HPP
#define STLXDM_DISPLAY_HPP

#include "layout.hpp"
#include "layout_store.hpp"
#include "presenter.hpp"

#include <stlx/gfxfb_abi.h>

#include <cstdint>
#include <memory>

/* /dev/gfxfb, opened once for all outputs, and a presenter for each
 * output stlxdm serves. It reads the file's events, handing each
 * present's end to its output's presenter and keeping the outputs that
 * output events named, and reads any output's state for the main loop
 * to follow.
 *
 * An output is served while it is lit and its presenter holds buffers at
 * its mode. When no lit output's buffers can be made at start for any
 * reason but -EBUSY, as on the boot display, one memcpy presenter serves
 * whichever output is primary instead, until stlxdm restarts. If that
 * presenter finds no lit primary or `GFXFB_GET_INFO` fails with EAGAIN,
 * `init()` waits for the next output event and tries the buffers again. */
class display {
public:
    /* Opens /dev/gfxfb and serves every lit output whose buffers can be
     * made, first waiting for output events while no output is lit and
     * primary, none lit fits the desktop's limit or `GFXFB_GET_INFO` fails
     * with EAGAIN for the memcpy presenter, a wait that never ends on a
     * display lighting no output. Returns 0, -EBUSY while another
     * program's file controls the buffers or reserves control, -1 when the
     * memcpy presenter cannot serve the primary output, or else the errno
     * of the step that failed, printing why. */
    int init();
    void shutdown();

    /* The descriptor that `poll` watches, which `GFXFB_GET_MODES` and
     * `GFXFB_SET_MODE` take too. Its own control of the buffers never
     * makes those calls fail with EBUSY. */
    int fd() const { return m_fd; }

    /* Reads `output`'s state into `info`. Returns 0 or a negative errno,
     * printing why. */
    int read_output(uint32_t output, gfxfb::gfxfb_output_info& info) const;

    /* Reads the file's events, waiting for one when none is queued. A
     * present's end goes to its output's presenter, and an output event
     * sets `output_changed()`. A read failing for any reason but a signal
     * ends every pending present with no latch kept. */
    void take_events();

    bool output_changed() const { return m_changed_outputs != 0; }

    /* Returns one bit per output that output events named since the last
     * call, and clears them, first waiting for every pending present's
     * end, since a present ending after its output changed could not be
     * told from one presented after the change. */
    uint32_t take_changed_outputs();

    /* Follows `output`'s state as read after its output event: keeps its
     * presenter, refreshed by `flip_presenter::relit()`, while it stays
     * lit at its size, and otherwise drops any presenter and makes one at
     * its size while the output reads lit. A lit output whose buffers
     * cannot be made or whose mode passes the desktop's limit stays
     * unserved, printing why. The memcpy fallback maps the primary again
     * instead, serving nothing while that fails or the primary passes the
     * limit. */
    void follow_output(uint32_t output, const gfxfb::gfxfb_output_info& info);

    /* `output`'s presenter, or null while it is not served. */
    presenter* presenter_of(uint32_t output) const;

    /* The output last read as gfxfb's primary. */
    uint32_t primary() const { return m_primary; }

    /* The monitor `output` last named while lit, kept while it relights. */
    const layout_store::identity& monitor(uint32_t output) const { return m_monitors[output]; }

    /* Whether `output` was last read dark while its driver lights it
     * again. */
    bool is_relighting(uint32_t output) const {
        return output < gfxfb::GFXFB_OUTPUTS_MAX && (m_relighting & (1u << output)) != 0;
    }

    /* Whether init fell back to the memcpy presenter. It stays until
     * `shutdown()`, and `presenter_of()` gives it only while it serves. */
    bool falls_back() const { return m_fallback != nullptr; }

private:
    int serve_outputs();
    bool fallback_fits() const;
    void note_output_event(uint32_t output);
    void note_state(uint32_t output, const gfxfb::gfxfb_output_info& info);

    int      m_fd = -1;
    uint32_t m_output_count = 0;
    uint32_t m_changed_outputs = 0;
    uint32_t m_primary = 0;
    uint32_t m_relighting = 0;

    /* Whether the fallback's last update succeeded within the limit. */
    bool     m_fallback_served = false;

    std::unique_ptr<flip_presenter>   m_presenters[gfxfb::GFXFB_OUTPUTS_MAX];
    std::unique_ptr<memcpy_presenter> m_fallback;

    layout_store::identity m_monitors[gfxfb::GFXFB_OUTPUTS_MAX];
};

#endif
