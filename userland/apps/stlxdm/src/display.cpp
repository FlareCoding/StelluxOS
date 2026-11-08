#include "display.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* A file holds at most one present event and one output event per output, so one read takes them all */
constexpr uint32_t EVENTS_PER_READ = 2 * gfxfb::GFXFB_OUTPUTS_MAX;

static bool is_lit(const gfxfb::gfxfb_output_info& info) {
    return (info.flags & gfxfb::GFXFB_OUTPUT_LIT) != 0;
}

static bool is_lit_primary(const gfxfb::gfxfb_output_info& info) {
    return is_lit(info) && (info.flags & gfxfb::GFXFB_OUTPUT_PRIMARY) != 0;
}

/* No buffer past the desktop's limit can be made, and the layout's sums rely on the bound */
static bool fits_desktop(uint32_t width, uint32_t height) {
    uint32_t limit = static_cast<uint32_t>(layout::DESKTOP_LIMIT);
    return width <= limit && height <= limit;
}

static void report_past_limit(uint32_t output, uint32_t width, uint32_t height) {
    printf("stlxdm: output %u's %ux%u passes the desktop's limit, leaving it unserved\r\n", output, width, height);
}

static void report_output(uint32_t output, const gfxfb::gfxfb_output_info& info) {
    if (!is_lit(info)) {
        bool relighting = (info.flags & gfxfb::GFXFB_OUTPUT_RELIGHTING) != 0;
        printf("stlxdm: output %u %s\r\n", output, relighting ? "relighting" : "dark");
        return;
    }

    unsigned long long millihertz = info.current.refresh_millihertz;
    const char* maker = info.maker[0] ? info.maker : "no monitor named";
    const char* space = info.name[0] ? " " : "";
    printf("stlxdm: output %u lit at %ux%u %llu.%03llu Hz, %s%s%s\r\n", output, info.current.width, info.current.height,
           millihertz / 1000, millihertz % 1000, maker, space, info.name);
}

/* Reads the events `fd` holds into `events`, which must hold `EVENTS_PER_READ` records, waiting for one when none is
 * queued. Returns how many it read, 0 after a signal, or -1 after printing why the read failed */
static int read_events(int fd, gfxfb::gfxfb_event* events) {
    ssize_t bytes = read(fd, events, EVENTS_PER_READ * sizeof(gfxfb::gfxfb_event));
    if (bytes >= 0) {
        return static_cast<int>(static_cast<size_t>(bytes) / sizeof(gfxfb::gfxfb_event));
    }

    if (errno == EINTR) {
        return 0;
    }

    printf("stlxdm: display event read failed: %s\r\n", strerror(errno));

    return -1;
}

int display::init() {
    m_fd = open("/dev/gfxfb", O_RDWR);
    if (m_fd < 0) {
        int error = errno;
        printf("stlxdm: display open failed: %s\r\n", strerror(error));
        return -error;
    }

    /* No present is pending yet, so a wait reads only output events */
    bool reported_wait = false;
    for (;;) {
        int rc = serve_outputs();
        if (rc != -EAGAIN) {
            if (rc != 0) {
                shutdown();
            }

            return rc;
        }

        if (!reported_wait) {
            printf("stlxdm: waiting for the display to light an output\r\n");
            reported_wait = true;
        }

        gfxfb::gfxfb_event events[EVENTS_PER_READ];
        read_events(m_fd, events);
    }
}

/* Serves every lit output whose buffers can be made, else the memcpy presenter when none can for a reason but -EBUSY.
 * Returns -EAGAIN in the cases `init()` names for its wait, else as `init()` does */
int display::serve_outputs() {
    gfxfb::gfxfb_output_info info;
    int rc = read_output(0, info);
    if (rc != 0) {
        return rc;
    }

    m_output_count = info.output_count;

    bool primary_lit = false;
    for (uint32_t output = 0; output < m_output_count && output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        rc = output == 0 ? 0 : read_output(output, info);
        if (rc != 0) {
            return rc;
        }

        note_state(output, info);
        primary_lit = primary_lit || is_lit_primary(info);
    }

    if (!primary_lit) {
        return -EAGAIN;
    }

    int failure = 0;
    bool served = false;
    for (uint32_t output = 0; output < m_output_count && output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        if (read_output(output, info) != 0 || !is_lit(info)) {
            continue;
        }

        if (!fits_desktop(info.current.width, info.current.height)) {
            report_past_limit(output, info.current.width, info.current.height);
            continue;
        }

        auto presenter = std::make_unique<flip_presenter>();
        rc = presenter->init(*this, info);
        if (rc == 0) {
            m_presenters[output] = std::move(presenter);
            served = true;
        } else if (failure == 0) {
            failure = rc;
        }
    }

    if (served) {
        return 0;
    }

    /* An output change since the first read, or the desktop's limit, left nothing to serve */
    if (failure == 0) {
        return -EAGAIN;
    }

    if (failure == -EBUSY) {
        return failure;
    }

    m_fallback = std::make_unique<memcpy_presenter>();
    rc = m_fallback->init(m_fd);
    if (rc == -EAGAIN) {
        m_fallback.reset();
        return rc;
    }

    m_fallback_served = rc == 0 && fallback_fits();

    return m_fallback_served ? 0 : -1;
}

bool display::fallback_fits() const {
    uint32_t width = m_fallback->width();
    uint32_t height = m_fallback->height();
    if (fits_desktop(width, height)) {
        return true;
    }

    report_past_limit(m_fallback->output(), width, height);

    return false;
}

/* Closing a file in control presents the outputs' base buffers again */
void display::shutdown() {
    for (auto& presenter : m_presenters) {
        if (presenter) {
            presenter->shutdown();
            presenter.reset();
        }
    }

    if (m_fallback) {
        m_fallback->shutdown();
        m_fallback.reset();
    }

    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

int display::read_output(uint32_t output, gfxfb::gfxfb_output_info& info) const {
    info = {};
    info.output = output;
    if (ioctl(m_fd, gfxfb::GFXFB_GET_OUTPUT, &info) < 0) {
        int error = errno;
        printf("stlxdm: output %u state read failed: %s\r\n", output, strerror(error));
        return -error;
    }

    return 0;
}

void display::take_events() {
    gfxfb::gfxfb_event events[EVENTS_PER_READ];
    int count = read_events(m_fd, events);
    if (count < 0) {
        for (auto& presenter : m_presenters) {
            if (presenter) {
                presenter->abandon_present();
            }
        }

        return;
    }

    /* No device drives output `GFXFB_OUTPUTS_MAX` or above, and dropping an event naming one keeps the shift defined
     * and the presenter lookup inside `m_presenters` */
    for (int i = 0; i < count; i++) {
        const gfxfb::gfxfb_event& event = events[i];
        if (event.output >= gfxfb::GFXFB_OUTPUTS_MAX) {
            continue;
        }

        if (event.type == gfxfb::GFXFB_EVENT_OUTPUT) {
            m_changed_outputs |= 1u << event.output;
            note_output_event(event.output);
        } else if (event.type == gfxfb::GFXFB_EVENT_PRESENT && m_presenters[event.output]) {
            m_presenters[event.output]->end_present(event.status, event.frame, event.timestamp_ns);
        }
    }
}

/* The memcpy presenter draws on whichever output is primary, which any output event may move */
void display::note_output_event(uint32_t output) {
    if (m_fallback) {
        m_fallback->note_output_event();
    } else if (m_presenters[output]) {
        m_presenters[output]->note_output_event();
    }
}

uint32_t display::take_changed_outputs() {
    wait_for_presents();

    uint32_t outputs = m_changed_outputs;
    m_changed_outputs = 0;

    return outputs;
}

void display::wait_for_presents() {
    for (auto& presenter : m_presenters) {
        while (presenter && presenter->present_pending()) {
            take_events();
        }
    }
}

void display::note_state(uint32_t output, const gfxfb::gfxfb_output_info& info) {
    if ((info.flags & gfxfb::GFXFB_OUTPUT_PRIMARY) != 0) {
        m_primary = output;
    }

    bool relighting = !is_lit(info) && (info.flags & gfxfb::GFXFB_OUTPUT_RELIGHTING) != 0;
    if (relighting) {
        m_relighting |= 1u << output;
    } else {
        m_relighting &= ~(1u << output);
    }

    layout_store::identity& monitor = m_monitors[output];
    if (is_lit(info)) {
        /* Zeroed past each NUL, as the store compares whole fields */
        monitor = {};
        strncpy(monitor.maker, info.maker, sizeof(monitor.maker) - 1);
        strncpy(monitor.name, info.name, sizeof(monitor.name) - 1);
        monitor.product = info.product;
        monitor.serial = info.serial;
    } else if (!relighting) {
        monitor = {};
    }
}

void display::follow_output(uint32_t output, const gfxfb::gfxfb_output_info& info) {
    if (output >= gfxfb::GFXFB_OUTPUTS_MAX) {
        return;
    }

    note_state(output, info);
    report_output(output, info);
    m_refused_outputs &= ~(1u << output);

    if (m_fallback) {
        m_fallback_served = m_fallback->update_output() == 0 && fallback_fits();
        return;
    }

    /* A presenter's buffers keep the size they were made at, so an output at another size gets a new one */
    std::unique_ptr<buffer_presenter>& served = m_presenters[output];
    bool same_size = served && served->width() == info.current.width && served->height() == info.current.height;
    if (is_lit(info) && same_size) {
        served->relit(info);
        return;
    }

    if (served) {
        served->shutdown();
        served.reset();
    }

    if (!is_lit(info)) {
        return;
    }

    if (!fits_desktop(info.current.width, info.current.height)) {
        report_past_limit(output, info.current.width, info.current.height);
        return;
    }

    served = make_presenter(info, m_maker);
    if (!served && m_maker) {
        m_refused_outputs |= 1u << output;
    }
}

/* A presenter for the lit output `info` describes, over output buffers
 * `maker` makes or gfxfb's own when null, or null after printing why */
std::unique_ptr<buffer_presenter> display::make_presenter(
    const gfxfb::gfxfb_output_info& info,
    output_buffer_maker* maker
) {
    if (maker) {
        auto presenter = std::make_unique<gpu_presenter>();
        if (presenter->init(*this, info, *maker) != 0) {
            return nullptr;
        }

        return presenter;
    }

    auto presenter = std::make_unique<flip_presenter>();
    if (presenter->init(*this, info) != 0) {
        return nullptr;
    }

    return presenter;
}

/* gfxfb keeps a dropped presenter's buffer on screen until the new one's first present replaces it */
int display::present_through(output_buffer_maker* maker) {
    if (m_fallback) {
        return -1;
    }

    wait_for_presents();

    std::unique_ptr<buffer_presenter> made[gfxfb::GFXFB_OUTPUTS_MAX];
    bool all_made = true;
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX && all_made; output++) {
        const std::unique_ptr<buffer_presenter>& served = m_presenters[output];
        if (!served) {
            continue;
        }

        gfxfb::gfxfb_output_info info;
        if (read_output(output, info) != 0) {
            all_made = false;
            break;
        }

        bool unchanged = is_lit(info) && info.current.width == served->width() &&
                         info.current.height == served->height();
        if (unchanged) {
            made[output] = make_presenter(info, maker);
        } else {
            printf("stlxdm: output %u changed before its presenter was replaced\r\n", output);
        }

        all_made = made[output] != nullptr;
    }

    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX && all_made && !maker; output++) {
        if ((m_refused_outputs & (1u << output)) == 0) {
            continue;
        }

        gfxfb::gfxfb_output_info info;
        bool is_read = read_output(output, info) == 0;
        bool serves = is_read && is_lit(info) && fits_desktop(info.current.width, info.current.height);
        if (serves) {
            made[output] = make_presenter(info, nullptr);
        }
    }

    std::unique_ptr<buffer_presenter>* dropped = all_made ? m_presenters : made;
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        if (dropped[output]) {
            dropped[output]->shutdown();
        }
    }

    if (!all_made) {
        return -1;
    }

    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        if (made[output]) {
            m_presenters[output] = std::move(made[output]);
        }
    }

    m_maker = maker;
    m_refused_outputs = 0;

    return 0;
}

presenter* display::presenter_of(uint32_t output) const {
    if (m_fallback) {
        return m_fallback_served && output == m_fallback->output() ? m_fallback.get() : nullptr;
    }

    return output < gfxfb::GFXFB_OUTPUTS_MAX ? m_presenters[output].get() : nullptr;
}
