#include "server.hpp"

#include <stlx/gfxfb_abi.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/ioctl.h>

static_assert(SWP_MODES_MAX == gfxfb::GFXFB_MODES_MAX, "proto.h names gfxfb's limit");
static_assert(SWP_OUTPUTS_MAX == gfxfb::GFXFB_OUTPUTS_MAX, "proto.h names gfxfb's limit");

/* How long a trial's mode stays unless its client keeps it */
constexpr uint64_t TRIAL_NS = 15ull * 1000000000ull;

constexpr uint64_t MILLIHERTZ_PER_HERTZ = 1000;

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

static swp_mode to_swp_mode(const gfxfb::gfxfb_mode& mode) {
    return { mode.width, mode.height, mode.refresh_millihertz };
}

static bool modes_equal(const swp_mode& a, const swp_mode& b) {
    return a.width == b.width && a.height == b.height && a.refresh_millihertz == b.refresh_millihertz;
}

/* An output's state, read as dark when the read fails */
static gfxfb::gfxfb_output_info read_output(int fd, uint32_t output) {
    gfxfb::gfxfb_output_info info = {};
    info.output = output;
    if (ioctl(fd, gfxfb::GFXFB_GET_OUTPUT, &info) < 0) {
        info = {};
    }

    return info;
}

/* Fills `msg` with an output's state and list */
static void read_modes(int fd, uint32_t output, swp_modes& msg) {
    msg = {};
    msg.output = output;
    msg.output_count = read_output(fd, 0).output_count;
    if (output >= msg.output_count) {
        return;
    }

    gfxfb::gfxfb_mode modes[gfxfb::GFXFB_MODES_MAX];
    gfxfb::gfxfb_mode_list list = {};
    list.output = output;
    list.capacity = gfxfb::GFXFB_MODES_MAX;
    list.modes = reinterpret_cast<uint64_t>(modes);
    if (ioctl(fd, gfxfb::GFXFB_GET_MODES, &list) < 0) {
        list.count = 0;
    }

    gfxfb::gfxfb_output_info info = read_output(fd, output);
    if ((info.flags & gfxfb::GFXFB_OUTPUT_LIT) != 0) {
        msg.flags |= SWP_OUTPUT_LIT;
    }

    if ((info.flags & gfxfb::GFXFB_OUTPUT_PRIMARY) != 0) {
        msg.flags |= SWP_OUTPUT_PRIMARY;
    }

    msg.current = to_swp_mode(info.current);
    msg.count = std::min(list.count, gfxfb::GFXFB_MODES_MAX);
    for (uint32_t i = 0; i < msg.count; i++) {
        msg.modes[i] = to_swp_mode(modes[i]);
    }
}

static const char* request_purpose(uint32_t outcome) {
    switch (outcome) {
    case SWP_MODE_SET:
        return "asked by";
    case SWP_MODE_TRIAL:
        return "trial asked by";
    case SWP_MODE_SET_BACK:
        return "trial set back for";
    case SWP_MODE_DEFAULT:
        return "default after a failed set-back for";
    default:
        return "previous mode, no buffers at the new one, for";
    }
}

void server::handle_get_modes(dm_client& c, const uint8_t* payload) {
    swp_get_modes m;
    memcpy(&m, payload, sizeof(m));

    swp_modes msg;
    read_modes(m_display->fd(), m.output, msg);
    send_to(c, SWP_MSG_MODES, &msg, sizeof(msg));

    /* The display drives at most `SWP_OUTPUTS_MAX` outputs, and the second bound keeps the shift defined */
    bool is_watchable = m.output < msg.output_count && m.output < SWP_OUTPUTS_MAX;
    if (is_watchable) {
        c.mode_watches |= 1u << m.output;
    }
}

void server::send_modes(dm_client& c, uint32_t output) {
    swp_modes msg;
    read_modes(m_display->fd(), output, msg);
    send_to(c, SWP_MSG_MODES, &msg, sizeof(msg));
}

void server::queue_mode_message(dm_client& c, uint16_t type, const uint8_t* payload) {
    dm_mode_message m;
    m.client = &c;
    m.type = type;

    if (type == SWP_MSG_SET_MODE) {
        swp_set_mode msg;
        memcpy(&msg, payload, sizeof(msg));
        m.output = msg.output;
        m.value = msg.flags;
        m.mode = msg.mode;
    } else {
        swp_keep_mode msg;
        memcpy(&msg, payload, sizeof(msg));
        m.output = msg.output;
        m.value = msg.keep;
    }

    m_mode_messages.push_back(m);
}

bool server::start_mode_request() {
    while (!m_next_request_waiting && !m_mode_messages.empty()) {
        dm_mode_message m = m_mode_messages.front();
        m_mode_messages.erase(m_mode_messages.begin());
        take_mode_message(m);
    }

    if (!m_next_request_waiting) {
        return false;
    }

    m_request = m_next_request;
    m_next_request_waiting = false;
    m_request_following = true;
    make_mode_request(m_request);

    return true;
}

/* Answers a message that needs no request of the display, or leaves its request waiting */
void server::take_mode_message(const dm_mode_message& m) {
    if (m.type == SWP_MSG_KEEP_MODE) {
        bool is_theirs = m_trial.running && m_trial.client == m.client && m_trial.output == m.output;
        if (m.value > 1) {
            answer_mode(m.client, m.output, 0, SWP_MODE_INVALID);
        } else if (!is_theirs) {
            answer_mode(m.client, m.output, 0, SWP_MODE_NO_TRIAL);
        } else if (m.value == 1) {
            m_trial.running = false;
            printf("stlxdm: output %u keeps its trial's mode\r\n", m.output);
            answer_mode(m.client, m.output, 0, SWP_MODE_KEPT);
        } else {
            printf("stlxdm: output %u's client did not keep its trial, setting it back\r\n", m.output);
            end_trial_with_set_back(m.client);
        }

        return;
    }

    if ((m.value & ~static_cast<uint32_t>(SWP_SET_MODE_TRIAL)) != 0) {
        answer_mode(m.client, m.output, 0, SWP_MODE_INVALID);
        return;
    }

    if (m_trial.running) {
        answer_mode(m.client, m.output, 0, SWP_MODE_BUSY);
        return;
    }

    bool is_trial = (m.value & SWP_SET_MODE_TRIAL) != 0;
    gfxfb::gfxfb_output_info info = read_output(m_display->fd(), m.output);

    dm_mode_request request;
    request.client = m.client;
    request.output = m.output;
    request.outcome = is_trial ? SWP_MODE_TRIAL : SWP_MODE_SET;
    request.mode = m.mode;
    request.previous = to_swp_mode(info.current);

    m_next_request = request;
    m_next_request_waiting = true;
}

void server::make_mode_request(dm_mode_request& request) {
    gfxfb::gfxfb_mode_request args = {};
    args.output = request.output;
    args.mode.width = request.mode.width;
    args.mode.height = request.mode.height;
    args.mode.refresh_millihertz = request.mode.refresh_millihertz;

    int fd = m_display->fd();
    request.result = ioctl(fd, gfxfb::GFXFB_SET_MODE, &args) < 0 ? -errno : 0;

    const char* client = request.client ? request.client->app_id : "nobody";
    printf("stlxdm: output %u asked for %ux%u at %llu.%03llu Hz (%s %s): %s\r\n",
           request.output, request.mode.width, request.mode.height,
           static_cast<unsigned long long>(request.mode.refresh_millihertz / MILLIHERTZ_PER_HERTZ),
           static_cast<unsigned long long>(request.mode.refresh_millihertz % MILLIHERTZ_PER_HERTZ),
           request_purpose(request.outcome), client, request.result == 0 ? "lit" : strerror(-request.result));

    /* The relight queues its output events before `GFXFB_SET_MODE` returns, so one read without waiting takes them all
     * and the caller follows them */
    pollfd pfd = { m_display->fd(), POLLIN, 0 };
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
        m_display->take_events();
    }
}

void server::finish_mode_request() {
    int fd = m_display->fd();
    const dm_mode_request& request = m_request;
    m_request_following = false;

    gfxfb::gfxfb_output_info info = read_output(fd, request.output);
    bool served = m_display->presenter_of(request.output) != nullptr;
    bool is_clients = request.outcome == SWP_MODE_SET || request.outcome == SWP_MODE_TRIAL;
    bool lacks_buffers = request.result == 0 && !served && (info.flags & gfxfb::GFXFB_OUTPUT_LIT) != 0;

    /* Only a client's request gets the previous mode after stlxdm cannot make buffers at the new one (the mode was set
     * and the output reads lit and not served), so a lack of buffers costs one more request at most */
    if (is_clients && lacks_buffers) {
        m_next_request = request;
        m_next_request.outcome = SWP_MODE_NO_BUFFERS;
        m_next_request.mode = request.previous;
        m_next_request_waiting = true;
        return;
    }

    if (request.outcome == SWP_MODE_SET_BACK && request.result != 0) {
        swp_modes list;
        read_modes(fd, request.output, list);
        if (list.count > 0) {
            m_next_request = request;
            m_next_request.outcome = SWP_MODE_DEFAULT;
            m_next_request.mode = list.modes[0];
            m_next_request_waiting = true;
            return;
        }
    }

    answer_mode(request.client, request.output, request.result, request.outcome);

    bool is_lit_trial = request.outcome == SWP_MODE_TRIAL && request.result == 0;
    if (is_lit_trial) {
        m_trial.running = true;
        m_trial.client = request.client;
        m_trial.output = request.output;
        m_trial.mode = request.mode;
        m_trial.previous = request.previous;
        m_trial.deadline_ns = now_ns() + TRIAL_NS;

        printf("stlxdm: output %u on trial, set back in 15 seconds unless kept\r\n", request.output);
    }
}

/* Whether `output` keeps its place for a client's mode request: while the request is followed, and while the request
 * for the previous mode, made when the client's mode left the output unserved, waits or is followed */
bool server::keeps_place_for_mode_request(uint32_t output) const {
    bool is_clients = m_request.outcome == SWP_MODE_SET || m_request.outcome == SWP_MODE_TRIAL ||
                      m_request.outcome == SWP_MODE_NO_BUFFERS;
    bool following = m_request_following && is_clients && m_request.output == output;
    bool waiting = m_next_request_waiting && m_next_request.outcome == SWP_MODE_NO_BUFFERS &&
                   m_next_request.output == output;
    return following || waiting;
}

void server::answer_mode(dm_client* c, uint32_t output, int32_t result, uint32_t outcome) {
    if (!c) {
        return;
    }

    dm_answer answer;
    answer.client = c;
    answer.type = SWP_MSG_MODE_RESULT;

    answer.mode.output = output;
    answer.mode.result = result;
    answer.mode.outcome = outcome;
    answer.mode.mode = to_swp_mode(read_output(m_display->fd(), output).current);
    m_answers.push_back(answer);
}

void server::end_trial_with_set_back(dm_client* answer_to) {
    m_trial.running = false;

    m_next_request = {};
    m_next_request.client = answer_to;
    m_next_request.output = m_trial.output;
    m_next_request.outcome = SWP_MODE_SET_BACK;
    m_next_request.mode = m_trial.previous;
    m_next_request_waiting = true;
}

void server::outputs_changed(uint32_t outputs) {
    bool is_trial_output = m_trial.running && (outputs & (1u << m_trial.output)) != 0;
    if (is_trial_output) {
        gfxfb::gfxfb_output_info info = read_output(m_display->fd(), m_trial.output);
        if (!modes_equal(to_swp_mode(info.current), m_trial.mode)) {
            m_trial.running = false;
            printf("stlxdm: output %u's trial ended, the display changed the output\r\n", m_trial.output);
        }
    }

    for (const auto& c : m_clients) {
        uint32_t watched = c->mode_watches & outputs;
        for (uint32_t output = 0; watched != 0; output++) {
            if ((watched & (1u << output)) != 0) {
                watched &= ~(1u << output);
                send_modes(*c, output);
            }
        }
    }
}

int64_t server::mode_timeout_ns(uint64_t now) const {
    if (!m_trial.running) {
        return -1;
    }

    return m_trial.deadline_ns > now ? static_cast<int64_t>(m_trial.deadline_ns - now) : 0;
}

void server::mode_tick(uint64_t now) {
    if (m_trial.running && now >= m_trial.deadline_ns) {
        printf("stlxdm: output %u's trial ran out, setting it back\r\n", m_trial.output);
        end_trial_with_set_back(m_trial.client);
    }
}

/* The client is freed once `drop_client` returns, so no queued message or answer may name it */
void server::forget_mode_client(dm_client& c) {
    auto is_theirs = [&c](const dm_mode_message& m) { return m.client == &c; };
    m_mode_messages.erase(std::remove_if(m_mode_messages.begin(), m_mode_messages.end(), is_theirs),
                          m_mode_messages.end());

    if (m_trial.running && m_trial.client == &c) {
        printf("stlxdm: output %u's trial ended with its client, setting it back\r\n", m_trial.output);
        end_trial_with_set_back(nullptr);
    }
}
