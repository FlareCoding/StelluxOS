#include "server.hpp"
#include "material.hpp"

#include "shell/chord.hpp"

#include <stlx/gpu_abi.h>
#include <stlx/proc.h>
#include <stlxgfx/image.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

constexpr const char* GPU_DEVICE_PATH = "/dev/gpu0";
constexpr const char* LAYOUTS_PATH = "/etc/stlxdm/layouts.conf";

static_assert(layout::MEMBERS_MAX == SWP_OUTPUTS_MAX, "a layout places every output the protocol lists");
static_assert(layout::DESKTOP_LIMIT == SWP_DESKTOP_LIMIT, "the layout keeps the protocol's limit");

/* Each attached GPU buffer holds two of stlxdm's handles, and 16 on
 * each of many clients pass the default limit */
constexpr rlim_t HANDLE_LIMIT = 65536;

/* Room for one attach's handles. The kernel fits (controllen -
 * sizeof(cmsghdr)) / 4, so CMSG_SPACE's padding would fit a fourth. */
constexpr size_t HANDLE_CONTROL_BYTES = CMSG_LEN(SWP_GPU_HANDLES * sizeof(int));

/* A retired GPU compositor's timeline is read at least this often */
constexpr int64_t RETIRED_QUERY_INTERVAL_NS = 100000000;

/* Spawns a detached app. args is an optional space-separated argument
 * string tokenized into a scratch copy, no quoting support. */
static void spawn_app(const char* path, const char* args) {
    char scratch[256] = "";
    if (args) {
        strncpy(scratch, args, sizeof(scratch) - 1);
    }

    const char* argv[16];
    int argc = stlxconf_split_args(scratch, argv, sizeof(argv) / sizeof(argv[0]));

    int handle = proc_exec(path, argc > 0 ? argv : nullptr);
    if (handle < 0) {
        printf("stlxdm: failed to spawn %s\r\n", path);
        return;
    }

    proc_detach(handle);
    printf("stlxdm: spawned %s\r\n", path);
}

/* Apps stlxdm spawns inherit the raised limits */
static void raise_handle_limit() {
    rlimit limit = { HANDLE_LIMIT, HANDLE_LIMIT };
    if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
        printf("stlxdm: raising the handle limit failed, errno %d, keeping the inherited limit\r\n", errno);
    }
}

static int open_gpu_device() {
    int fd = open(GPU_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        printf("stlxdm: no GPU buffers, opening %s failed, errno %d\r\n", GPU_DEVICE_PATH, errno);
        return -1;
    }

    printf("stlxdm: taking GPU buffers of %s\r\n", GPU_DEVICE_PATH);

    return fd;
}

/* A context create meets EIO once the GPU failed */
static bool is_gpu_failed(int gpu_fd) {
    gpu_context_args context = {};
    if (ioctl(gpu_fd, GPU_CONTEXT_CREATE, &context) == 0) {
        close(context.context);
        return false;
    }

    return errno == EIO;
}

/* Queues the handles of a read's one SCM_RIGHTS message, closing any
 * past the queue's room. Returns false when some were dropped. */
static bool queue_received_handles(dm_client& c, const msghdr& msg) {
    bool kept_all = (msg.msg_flags & MSG_CTRUNC) == 0;

    const cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len < CMSG_LEN(0)) {
        return kept_all;
    }

    size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (size_t i = 0; i < count; i++) {
        int handle;
        memcpy(&handle, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(handle));
        if (c.queued_handle_count == dm_client::QUEUED_HANDLES_MAX) {
            close(handle);
            kept_all = false;
            continue;
        }

        c.queued_handles[c.queued_handle_count++] = handle;
    }

    return kept_all;
}

/* Whether the unhandled bytes may begin the ATTACH_GPU_BUFFER that
 * queued handles wait for. A lone byte, short of a type, may. */
static bool may_begin_gpu_attach(const dm_client& c) {
    if (c.rd_have == 0) {
        return false;
    }

    if (c.rd_have < sizeof(uint16_t)) {
        return true;
    }

    uint16_t type;
    memcpy(&type, c.rd_buf, sizeof(type));

    return type == SWP_MSG_ATTACH_GPU_BUFFER;
}

/* The configured wallpaper decoded, or null when there is none or it
 * fails to load */
static stlxgfx_surface_t* load_wallpaper(const stlxconf_t& conf) {
    if (!conf.wallpaper[0]) {
        return nullptr;
    }

    stlxgfx_surface_t* image = stlxgfx_load_image(conf.wallpaper);
    if (!image) {
        printf("stlxdm: failed to load wallpaper %s\r\n", conf.wallpaper);
    }

    return image;
}

dm_wallpaper::~dm_wallpaper() {
    stlxgfx_destroy_surface(m_image);
}

const stlxgfx_surface_t* dm_wallpaper::image() {
    if (!m_decoded) {
        m_image = load_wallpaper(m_conf);
        m_decoded = true;
    }

    return m_image;
}

/* Builds an output-sized backdrop: the wallpaper `image` center cropped
 * and scaled to cover the output, or the flat background color without one */
static stlxgfx_surface_t* build_backdrop(const stlxconf_t& conf,
                                         const stlxgfx_surface_t* image,
                                         uint32_t width,
                                         uint32_t height) {
    stlxgfx_surface_t* backdrop =
        stlxgfx_create_surface(width, height, 32, 16, 8, 0);
    if (!backdrop) {
        return nullptr;
    }
    stlxgfx_clear(backdrop, conf.bg_color);

    if (!image) {
        return backdrop;
    }

    /* Center-crop the source to the output's aspect ratio, then scale */
    uint64_t img_w = image->width;
    uint64_t img_h = image->height;
    uint32_t crop_w;
    uint32_t crop_h;
    if (img_w * height > img_h * width) {
        crop_h = static_cast<uint32_t>(img_h);
        crop_w = static_cast<uint32_t>(img_h * width / height);
    } else {
        crop_w = static_cast<uint32_t>(img_w);
        crop_h = static_cast<uint32_t>(img_w * height / width);
    }
    if (crop_w == 0) crop_w = 1;
    if (crop_h == 0) crop_h = 1;

    int32_t crop_x = static_cast<int32_t>((img_w - crop_w) / 2);
    int32_t crop_y = static_cast<int32_t>((img_h - crop_h) / 2);
    stlxgfx_blit_scaled(backdrop, 0, 0, width, height,
                        image, crop_x, crop_y, crop_w, crop_h);

    return backdrop;
}

/* A copy of `source` in memory of its own, null when memory ran out */
static stlxgfx_surface_t* copy_surface(const stlxgfx_surface_t* source) {
    stlxgfx_surface_t* copy = stlxgfx_create_surface(source->width, source->height, 32, 16, 8, 0);
    if (copy) {
        stlxgfx_blit(copy, 0, 0, source, 0, 0, source->width, source->height);
    }

    return copy;
}

static bool same_layout(const layout& a, const layout& b) {
    if (a.count() != b.count() || a.main_output() != b.main_output()) {
        return false;
    }

    for (uint32_t i = 0; i < a.count(); i++) {
        const damage_list::rect& ra = a.at(i).rect;
        const damage_list::rect& rb = b.at(i).rect;
        bool same_place = ra.x == rb.x && ra.y == rb.y && ra.w == rb.w && ra.h == rb.h;
        if (a.at(i).output != b.at(i).output || !same_place) {
            return false;
        }
    }

    return true;
}

static void report_default(layout::outcome placed) {
    if (placed == layout::outcome::defaulted) {
        printf("stlxdm: the layout left outputs overlapping, apart or past the desktop's limit, using the default\r\n");
    }
}

int server::init(display* disp, const stlxconf_t* conf) {
    m_display = disp;
    m_conf = conf;
    m_compose_fault = conf->compose_fault;
    m_compositor = conf->compositor;

    m_store.read(LAYOUTS_PATH);

    layout::member members[layout::MEMBERS_MAX];
    layout::arrangement stored;
    uint32_t count = gather_members(members);
    layout::outcome placed = m_layout.rebuild(members, count, disp->primary(), stored_layout(members, count, stored));
    if (placed == layout::outcome::kept) {
        return -1;
    }

    report_default(placed);

    sync_outputs();

    if (m_cursor.init() != 0) {
        return -1;
    }

    m_gpu_fd = open_gpu_device();
    choose_compositor();
    material::configure(conf->glass_level, conf->glass_tint);
    m_glass_level = conf->glass_level;
    m_glass_tint = conf->glass_tint;

    dm_wallpaper wallpaper(*conf);
    dm_conf_state state;
    if (build_conf_state(state, wallpaper) != 0) {
        return -1;
    }
    adopt_conf_state(std::move(state));
    m_layout.set_shell_rows(dm_panels::BAR_H, m_panels->dock_y());
    refresh_backdrops(false, wallpaper);

    /* The load needs two samples, so the first is taken before the splash
     * for the bar's first frame after the splash to show a load */
    m_panels->clock_tick();

    const damage_list::rect& main = m_layout.find(m_layout.main_output())->rect;
    m_cursor_x = main.x + main.w / 2;
    m_cursor_y = main.y + main.h / 2;
    update_generation();

    /* The first tick paints the whole desktop over the boot contents */
    m_damage.add_full();

    return 0;
}

void server::choose_compositor() {
    if (m_conf->compositor == STLXCONF_COMPOSITOR_CPU) {
        printf("stlxdm: composing on the CPU, the config's compositor is cpu\r\n");
        return;
    }

    if (m_gpu_fd < 0) {
        printf("stlxdm: composing on the CPU, no %s\r\n", GPU_DEVICE_PATH);
        return;
    }

    m_gpu_compositor = gpu_compositor::create(m_gpu_fd);
}

void server::switch_to_gpu() {
    if (!m_gpu_compositor) {
        return;
    }

    m_compares_frames = m_conf->compare_frames;
    const char* refusal = m_display->falls_back() ? "the display has no buffers" : nullptr;
    if (!refusal) {
        m_gpu_backdrop = copy_backdrop(m_backdrop, m_backdrop->width, m_backdrop->height);
        m_gpu_frost = copy_frost(m_frost);
        m_gpu_capture = m_gpu_compositor->make_picture(static_cast<size_t>(m_backdrop->width) * m_backdrop->height * 4);
        bool made = m_gpu_backdrop != nullptr && m_gpu_frost != nullptr && m_gpu_capture != nullptr;
        for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX && made; output++) {
            made = !m_outputs[output].pres || make_output_pictures(output);
        }

        refusal = made ? nullptr : "a picture's memory ran out";
    }

    if (!refusal && m_display->present_through(m_gpu_compositor.get()) != 0) {
        refusal = "an output has no GPU presenter";
    }

    if (refusal) {
        m_gpu_backdrop.reset();
        m_gpu_frost.reset();
        m_gpu_capture.reset();
        for (dm_output& out : m_outputs) {
            out.pictures = {};
        }

        drop_window_imports();
        m_gpu_compositor.reset();
        printf("stlxdm: composing on the CPU, %s\r\n", refusal);
        return;
    }

    m_panels->rebind(m_gpu_backdrop->surface(), m_gpu_frost->surface());
    m_panels->use_picture_maker(m_gpu_compositor.get());
    m_power->capture_into(m_gpu_capture->pixels());

    stlxgfx_destroy_surface(m_backdrop);
    stlxgfx_destroy_surface(m_frost);
    m_backdrop = nullptr;
    m_frost = nullptr;

    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        dm_output& out = m_outputs[output];
        stlxgfx_destroy_surface(out.backdrop);
        stlxgfx_destroy_surface(out.frost);
        out.backdrop = nullptr;
        out.frost = nullptr;
        if (out.pres) {
            out.pres = m_display->presenter_of(output);
        }
    }

    m_gpu_composes = true;
    m_damage.add_full();
    printf("stlxdm: composing on the GPU%s\r\n", m_compares_frames ? ", comparing each frame with the CPU's" : "");
}

/* Retires the GPU compositor and moves every served output to the CPU compositor on a flip presenter, or stops
 * composing when a flip presenter or a copy of the main output's backdrop or frost fails */
void server::switch_to_cpu(const char* reason) {
    m_gpu_compositor->retire();
    m_panels->use_picture_maker(nullptr);

    stlxgfx_surface_t* backdrop = copy_surface(m_gpu_backdrop->surface());
    stlxgfx_surface_t* frost = copy_surface(m_gpu_frost->surface());
    const char* refusal = nullptr;
    if (!backdrop) {
        refusal = "no memory was left for the main backdrop's copy";
    } else if (!frost) {
        refusal = "no memory was left for the main output's frost copy";
    }

    if (!refusal && m_display->present_through(nullptr) != 0) {
        refusal = "an output has no flip presenter";
        show_cursor();
    }

    if (refusal) {
        stlxgfx_destroy_surface(backdrop);
        stlxgfx_destroy_surface(frost);
        printf("stlxdm: composing stopped, %s, and the CPU cannot take over since %s\r\n", reason, refusal);
        m_gpu_stopped = true;

        release_unmappable_buffers();
        follow_reader_in_gpu_info();
        reap_retired_compositor();
        return;
    }

    m_panels->rebind(backdrop, frost);
    m_power->capture_into(nullptr);
    m_gpu_capture.reset();

    m_backdrop = backdrop;
    m_frost = frost;
    m_gpu_backdrop.reset();
    m_gpu_frost.reset();

    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        dm_output& out = m_outputs[output];
        if (out.pictures.backdrop && !out.backdrop_failed) {
            out.backdrop = copy_surface(out.pictures.backdrop->surface());
            if (!out.backdrop) {
                printf("stlxdm: output %u's backdrop could not be copied, showing the background color\r\n", output);
            }
        }

        if (out.pictures.frost) {
            out.frost = copy_surface(out.pictures.frost->surface());
            if (!out.frost) {
                printf("stlxdm: output %u's frost could not be copied, showing no glass\r\n", output);
            }
        }

        out.pictures = {};
        if (out.pres) {
            out.pres = m_display->presenter_of(output);
        }
    }

    drop_window_imports();
    m_gpu_composes = false;
    m_retired_compositor = std::move(m_gpu_compositor);

    release_unmappable_buffers();
    follow_reader_in_gpu_info();
    m_damage.add_full();
    printf("stlxdm: composing on the CPU, %s\r\n", reason);

    show_cursor();
    reap_retired_compositor();
}

void server::reap_retired_compositor() {
    gpu_compositor* retired = retired_compositor();
    if (!retired) {
        return;
    }

    const char* cause = nullptr;
    switch (retired->last_compose_state()) {
    case compose_state::ended:
        cause = "its last compose ended";
        break;
    case compose_state::running:
        return;
    case compose_state::lost:
        cause = "its device was lost";
        break;
    }

    if (retired == m_gpu_compositor.get()) {
        m_gpu_compositor->end_retirement();
        printf("stlxdm: the stopped GPU compositor freed the memory it kept for its last compose, %s\r\n", cause);
        return;
    }

    m_retired_compositor.reset();
    printf("stlxdm: the retired GPU compositor is destroyed, %s\r\n", cause);

    if (m_gpu_switch_waits) {
        m_gpu_switch_waits = false;
        switch_to_gpu_at_reload();
    }
}

int64_t server::retired_timeout_ns() const {
    return retired_compositor() ? RETIRED_QUERY_INTERVAL_NS : -1;
}

gpu_compositor* server::retired_compositor() const {
    if (m_retired_compositor) {
        return m_retired_compositor.get();
    }

    return m_gpu_stopped && m_gpu_compositor->is_retired() ? m_gpu_compositor.get() : nullptr;
}

/* Has the next GPU compose make the failure the config's `compose_fault` names, once each time a reload changes it
 * from none, unless the GPU composes nothing or refuses the failure */
void server::follow_compose_fault() {
    bool changed_from_none = m_compose_fault == STLXCONF_COMPOSE_FAULT_NONE &&
                             m_conf->compose_fault != STLXCONF_COMPOSE_FAULT_NONE;
    m_compose_fault = m_conf->compose_fault;
    if (!changed_from_none) {
        return;
    }

    if (!composes_on_gpu()) {
        printf("stlxdm: compose_fault is ignored, the GPU composes nothing\r\n");
        return;
    }

    compose_failure failure = compose_failure::unmapped_read;
    const char* effect = "reads an address no context maps";
    switch (m_compose_fault) {
    case STLXCONF_COMPOSE_FAULT_NONE:
    case STLXCONF_COMPOSE_FAULT_FAULT:
        break;
    case STLXCONF_COMPOSE_FAULT_HANG:
        failure = compose_failure::spin;
        effect = "spins past its deadline";
        break;
    }

    if (!m_gpu_compositor->fail_next_compose(failure)) {
        printf("stlxdm: compose_fault=fault is ignored, this GPU's contexts may map address 0\r\n");
        return;
    }

    printf("stlxdm: the next GPU compose %s, as compose_fault asks\r\n", effect);
}

/* Switches compositors when a reload changes the config's `compositor`, unless composing stopped: to cpu while the GPU
 * composes, or to auto once no GPU compositor is retired, so two devices' output buffers are never held at once */
void server::follow_compositor() {
    bool changed = m_compositor != m_conf->compositor;
    m_compositor = m_conf->compositor;
    if (!changed) {
        return;
    }

    if (m_gpu_stopped) {
        printf("stlxdm: composing stopped, a change of compositor takes effect at the next start\r\n");
        return;
    }

    m_gpu_switch_waits = false;
    switch (m_compositor) {
    case STLXCONF_COMPOSITOR_CPU:
        if (composes_on_gpu()) {
            switch_to_cpu("the config's compositor is cpu");
        }

        return;
    case STLXCONF_COMPOSITOR_AUTO:
        break;
    }

    if (retired_compositor()) {
        printf("stlxdm: the switch to the GPU waits for the retired GPU compositor\r\n");
        m_gpu_switch_waits = true;
        return;
    }

    switch_to_gpu_at_reload();
}

/* With clients connected, a reload's GPU compositor imports every attached buffer but release-only records first. A
 * creation or a switch refused on a failed GPU ends taking GPU buffers, as an import that finds it failed does */
void server::switch_to_gpu_at_reload() {
    choose_compositor();
    if (!m_gpu_compositor) {
        if (m_gpu_fd >= 0 && is_gpu_failed(m_gpu_fd)) {
            stop_taking_gpu_buffers();
        }

        return;
    }

    if (!import_attached_buffers()) {
        return;
    }

    switch_to_gpu();
    if (composes_on_gpu()) {
        follow_reader_in_gpu_info();
    } else if (is_gpu_failed(m_gpu_fd)) {
        stop_taking_gpu_buffers();
    }

    /* Dropping a presenter hides its cursor: a switch drops the flip presenters, a refused one the GPU ones it made */
    show_cursor();
}

int server::serve() {
    raise_handle_limit();

    if (m_gpu_fd >= 0) {
        m_gpu_info.flags = accepting_gpu_flags();
    }

    m_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listen_fd < 0) {
        return -1;
    }

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SWP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(SWP_SOCKET_PATH);
    if (bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0 ||
        listen(m_listen_fd, 8) != 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
        return -1;
    }

    fcntl(m_listen_fd, F_SETFL, O_NONBLOCK);

    /* The config's autostart entries, or a lone terminal without any */
    if (m_conf->autostart_count > 0) {
        for (uint32_t i = 0; i < m_conf->autostart_count; i++) {
            spawn_app(m_conf->autostart[i].path, m_conf->autostart[i].args);
        }
    } else {
        spawn_app("/bin/stlxterm", nullptr);
    }

    return 0;
}

/* Builds every config-derived piece at the main output's size into fresh
 * objects, touching nothing running, so a failure leaves no trace */
int server::build_conf_state(dm_conf_state& out, dm_wallpaper& wallpaper) {
    const damage_list::rect& main = m_layout.find(m_layout.main_output())->rect;
    uint32_t width = static_cast<uint32_t>(main.w);
    uint32_t height = static_cast<uint32_t>(main.h);

    out.backdrop = build_backdrop(*m_conf, wallpaper.image(), width, height);
    if (!out.backdrop) {
        return -1;
    }

    out.panels = std::make_unique<dm_panels>();
    out.power = std::make_unique<dm_power>();
    if (m_gpu_composes) {
        out.gpu_capture = m_gpu_compositor->make_picture(static_cast<size_t>(width) * height * 4);
    }

    /* The frost of the wallpaper, which every glass surface on the output is drawn from */
    out.frost = material::build_frost(out.backdrop, m_glass_tint);
    uint8_t* capture = out.gpu_capture ? out.gpu_capture->pixels() : nullptr;
    bool ready = out.frost && (!m_gpu_composes || capture) &&
                 out.panels->init(width, height, *m_conf, out.backdrop, out.frost, m_stats) == 0 &&
                 out.power->init(width, height, capture) == 0;
    if (ready && m_gpu_composes) {
        out.gpu_backdrop = copy_backdrop(out.backdrop, width, height);
        out.gpu_frost = copy_frost(out.frost);
        ready = out.gpu_backdrop != nullptr && out.gpu_frost != nullptr;
    }

    if (!ready) {
        out.panels->shutdown();
        out.power->shutdown();

        stlxgfx_destroy_surface(out.backdrop);
        stlxgfx_destroy_surface(out.frost);
        out.backdrop = nullptr;
        out.frost = nullptr;
        return -1;
    }

    if (out.gpu_backdrop) {
        out.panels->rebind(out.gpu_backdrop->surface(), out.gpu_frost->surface());
        stlxgfx_destroy_surface(out.backdrop);
        stlxgfx_destroy_surface(out.frost);
        out.backdrop = nullptr;
        out.frost = nullptr;
    }

    out.panels->on_unminimize = [this](const stlxconf_pin_t& pin) {
        return unminimize_app_window(pin);
    };

    out.panels->on_launch = [](const stlxconf_pin_t& pin) {
        spawn_app(pin.path, pin.args);
    };

    out.panels->on_spawn = [](const char* path, const char* args) {
        spawn_app(path, args);
    };

    out.panels->on_power = [this]() {
        m_power->open();
        m_damage.add_full();
    };

    out.panels->use_picture_maker(m_gpu_composes ? m_gpu_compositor.get() : nullptr);

    /* Exec shortcuts with parseable chords become live hotkeys */
    for (uint32_t i = 0; i < m_conf->shortcut_count; i++) {
        const stlxconf_shortcut_t& sc = m_conf->shortcuts[i];
        if (strcmp(sc.action, "exec") != 0 || !sc.path[0]) {
            continue;
        }

        dm_hotkey hk;
        if (parse_chord(sc.key, &hk.mods, &hk.usage)) {
            hk.path = sc.path;
            hk.args = sc.args;
            out.hotkeys.push_back(hk);
        }
    }

    return 0;
}

/* Retires the running config state and moves the built one in. The built
 * overlay is closed, so the running one issues a committed operation. */
void server::adopt_conf_state(dm_conf_state&& next) {
    if (m_panels) {
        m_panels->shutdown();
    }
    if (m_power) {
        m_power->run_action_now();
        m_power->shutdown();
    }
    stlxgfx_destroy_surface(m_backdrop);
    stlxgfx_destroy_surface(m_frost);

    m_panels = std::move(next.panels);
    m_power = std::move(next.power);
    m_gpu_capture = std::move(next.gpu_capture);
    m_hotkeys = std::move(next.hotkeys);

    m_backdrop = next.backdrop;
    m_gpu_backdrop = std::move(next.gpu_backdrop);
    m_frost = next.frost;
    m_gpu_frost = std::move(next.gpu_frost);
    next.backdrop = nullptr;
    next.frost = nullptr;
}

/* Gives each served output but the main one a backdrop at its size and its frost, building a failed one again only at
 * a new presenter or size or with `rebuild_all` */
void server::refresh_backdrops(bool rebuild_all, dm_wallpaper& wallpaper) {
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        dm_output& out = m_outputs[output];
        bool needs_own = out.pres && output != m_layout.main_output();
        const stlxgfx_surface_t* backdrop = out.pictures.backdrop ? out.pictures.backdrop->surface() : out.backdrop;
        bool fits = backdrop &&
                    static_cast<int32_t>(backdrop->width) == out.place.w &&
                    static_cast<int32_t>(backdrop->height) == out.place.h;
        if (needs_own && (fits || out.backdrop_failed) && !rebuild_all) {
            continue;
        }

        stlxgfx_destroy_surface(out.backdrop);
        out.backdrop = nullptr;
        out.pictures.backdrop.reset();
        out.backdrop_failed = false;

        stlxgfx_destroy_surface(out.frost);
        out.frost = nullptr;
        out.pictures.frost.reset();
        if (!needs_own) {
            continue;
        }

        uint32_t width = static_cast<uint32_t>(out.place.w);
        uint32_t height = static_cast<uint32_t>(out.place.h);
        out.backdrop = build_backdrop(*m_conf, wallpaper.image(), width, height);
        if (!out.backdrop) {
            out.backdrop_failed = true;
            printf("stlxdm: output %u's backdrop could not be built, showing the background color\r\n", output);
        }

        /* An output whose frost could not be built composes its windows without glass */
        out.frost = out.backdrop ? material::build_frost(out.backdrop, m_glass_tint) : nullptr;
        if (out.backdrop && !out.frost) {
            printf("stlxdm: output %u's frost could not be built, showing no glass\r\n", output);
        }

        if (m_gpu_composes) {
            out.pictures.backdrop = copy_backdrop(out.backdrop, width, height);
            if (out.pictures.backdrop) {
                stlxgfx_destroy_surface(out.backdrop);
                out.backdrop = nullptr;
            }

            out.pictures.frost = copy_frost(out.frost);
            if (out.pictures.frost) {
                stlxgfx_destroy_surface(out.frost);
                out.frost = nullptr;
            }
        }
    }
}

/* A frost picture holding `frost`'s pixels, null without one or when memory ran out */
std::unique_ptr<frost_picture> server::copy_frost(const stlxgfx_surface_t* frost) {
    if (!frost) {
        return nullptr;
    }

    return copy_backdrop(frost, frost->width, frost->height);
}

/* A backdrop picture holding `backdrop`'s pixels, or the background color
 * without one, null when memory ran out */
std::unique_ptr<backdrop_picture> server::copy_backdrop(
    const stlxgfx_surface_t* backdrop,
    uint32_t width,
    uint32_t height
) {
    std::unique_ptr<backdrop_picture> picture = backdrop_picture::make(*m_gpu_compositor, width, height);
    if (!picture) {
        return nullptr;
    }

    if (backdrop) {
        stlxgfx_blit(picture->surface(), 0, 0, backdrop, 0, 0, width, height);
    } else {
        stlxgfx_clear(picture->surface(), m_conf->bg_color);
    }

    return picture;
}

/* Makes the pictures `output`'s record lacks under the GPU compositor, the backdrop and frost of any output but the
 * main one copies of its own. False when memory ran out */
bool server::make_output_pictures(uint32_t output) {
    dm_output& out = m_outputs[output];
    dm_output_pictures& pictures = out.pictures;
    uint32_t width = out.pres->width();
    uint32_t height = out.pres->height();

    bool is_main = output == m_layout.main_output();
    if (!is_main && !pictures.backdrop) {
        pictures.backdrop = copy_backdrop(out.backdrop, width, height);
    }

    if (!is_main && !pictures.frost) {
        pictures.frost = copy_frost(out.frost);
    }

    /* switch_to_gpu keeps the CPU backdrops until it commits */
    bool drops_cpu_backdrop = m_gpu_composes && pictures.backdrop && out.backdrop;
    if (drops_cpu_backdrop) {
        stlxgfx_destroy_surface(out.backdrop);
        out.backdrop = nullptr;
    }

    bool drops_cpu_frost = m_gpu_composes && pictures.frost && out.frost;
    if (drops_cpu_frost) {
        stlxgfx_destroy_surface(out.frost);
        out.frost = nullptr;
    }

    if (!pictures.lower_top) {
        pictures.lower_top = top_picture::make(*m_gpu_compositor, width, height);
    }

    if (!pictures.upper_top) {
        pictures.upper_top = top_picture::make(*m_gpu_compositor, width, height);
    }

    if (!pictures.compose) {
        pictures.compose = m_gpu_compositor->make_compose_buffers();
    }

    if (m_compares_frames && !pictures.readback) {
        pictures.readback = m_gpu_compositor->make_picture(
            static_cast<size_t>(gpu_compositor::output_pitch(width)) * height
        );
    }

    if (m_compares_frames && !pictures.comparison) {
        pictures.comparison = m_gpu_compositor->make_picture(static_cast<size_t>(width) * height * 4);
    }

    bool compares = !m_compares_frames || (pictures.readback && pictures.comparison);
    bool has_backdrop_and_frost = is_main || (pictures.backdrop && (pictures.frost || !out.frost));
    return has_backdrop_and_frost && pictures.lower_top && pictures.upper_top && pictures.compose && compares;
}

/* Gives every served output the pictures it lacks while the GPU composes */
void server::make_served_pictures() {
    if (!composes_on_gpu()) {
        return;
    }

    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        if (m_outputs[output].pres && !make_output_pictures(output)) {
            switch_to_cpu("a picture's memory ran out");
            return;
        }
    }
}

/* The outputs the layout places: every served output at its mode's size,
 * and at its last size every member relighting or kept for a mode request */
uint32_t server::gather_members(layout::member* members) const {
    uint32_t count = 0;
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        const presenter* pres = m_display->presenter_of(output);
        const layout::place* place = m_layout.find(output);
        bool keeps_place = m_display->is_relighting(output) || keeps_place_for_mode_request(output);
        if (pres) {
            members[count++] = { output, static_cast<int32_t>(pres->width()), static_cast<int32_t>(pres->height()) };
        } else if (keeps_place && place) {
            members[count++] = { output, place->rect.w, place->rect.h };
        }
    }

    return count;
}

/* The stored layout for `members`' monitors when they differ from the last ones the layout was built for, keeping
 * them as the last unless there are none, which leaves the layout as it was */
const layout::arrangement* server::stored_layout(const layout::member* members, uint32_t count,
                                                 layout::arrangement& out) {
    if (count == 0) {
        return nullptr;
    }

    layout_store::member monitors[layout::MEMBERS_MAX];
    bool differs = count != m_monitor_count;
    for (uint32_t i = 0; i < count; i++) {
        monitors[i] = { members[i].output, m_display->monitor(members[i].output) };
        bool same = monitors[i].output == m_monitors[i].output && monitors[i].monitor == m_monitors[i].monitor;
        differs = differs || !same;
    }

    std::copy(monitors, monitors + count, m_monitors);
    m_monitor_count = count;
    return differs && m_store.match(monitors, count, out) ? &out : nullptr;
}

/* Takes each output's presenter and place, clearing the record of an output no longer served and all but the backdrop
 * of one with a new presenter or size, and restarting the cascade of an output that left the layout */
void server::sync_outputs() {
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        dm_output& out = m_outputs[output];
        presenter* pres = m_display->presenter_of(output);
        const layout::place* place = m_layout.find(output);
        if (!place) {
            m_window_count[output] = 0;
        }

        if (!pres || !place) {
            stlxgfx_destroy_surface(out.backdrop);
            stlxgfx_destroy_surface(out.frost);
            out = {};
            continue;
        }

        /* A presenter made at another size can take the address of the one it replaced */
        bool resized = out.place.w != place->rect.w || out.place.h != place->rect.h;
        if (out.pres != pres || resized) {
            stlxgfx_surface_t* backdrop = out.backdrop;
            stlxgfx_surface_t* frost = out.frost;
            std::unique_ptr<backdrop_picture> gpu_backdrop = std::move(out.pictures.backdrop);
            std::unique_ptr<frost_picture> gpu_frost = std::move(out.pictures.frost);
            out = {};

            out.backdrop = backdrop;
            out.frost = frost;
            out.pictures.backdrop = std::move(gpu_backdrop);
            out.pictures.frost = std::move(gpu_frost);
        }

        out.pres = pres;
        out.place = place->rect;
    }
}

void server::spread_damage() {
    if (m_damage.empty()) {
        return;
    }

    for (dm_output& out : m_outputs) {
        if (!out.pres) {
            continue;
        }

        if (m_damage.full()) {
            out.damage.add_full();
            continue;
        }

        for (uint32_t i = 0; i < m_damage.count(); i++) {
            damage_list::rect r = m_damage.at(i);
            r.x -= out.place.x;
            r.y -= out.place.y;
            if (damage_list::clip(r, out.place.w, out.place.h)) {
                out.damage.add(r.x, r.y, r.w, r.h);
            }
        }
    }

    m_damage.clear();
}

bool server::compose_pending(uint32_t output) const {
    const dm_output& out = m_outputs[output];
    if (!out.pres) {
        return false;
    }

    bool is_main = output == m_layout.main_output();
    bool shell_pending = is_main && (m_panels->dirty() || m_power->animating());
    return !out.damage.empty() || has_latch_work(output) || has_configure_work() || shell_pending;
}

void server::sync_shell() {
    dm_client* owner = m_focus ? window_owner(m_focus) : nullptr;
    m_panels->show_focused_app(owner ? owner->app_id : nullptr);

    uint64_t running = 0;
    for (const auto& client : m_clients) {
        int32_t launcher = m_panels->launcher_of(client->app_id);
        if (launcher >= 0 && !client->windows.empty()) {
            running |= 1ull << launcher;
        }
    }

    m_panels->show_running_launchers(running, owner ? m_panels->launcher_of(owner->app_id) : -1);
}

void server::reload_config() {
    /* `m_conf` holds the new pins already, so this runs even when the rebuild below fails */
    unminimize_windows_without_launcher();

    follow_compositor();
    follow_compose_fault();

    stlxconf_glass_level_t running_level = m_glass_level;
    stlxconf_glass_tint_t running_tint = m_glass_tint;
    m_glass_level = m_conf->glass_level;
    m_glass_tint = m_conf->glass_tint;
    material::configure(m_glass_level, m_glass_tint);

    dm_wallpaper wallpaper(*m_conf);
    dm_conf_state state;
    if (build_conf_state(state, wallpaper) != 0) {
        m_glass_level = running_level;
        m_glass_tint = running_tint;
        material::configure(m_glass_level, m_glass_tint);
        printf("stlxdm: config reload failed, keeping the running desktop\r\n");
        return;
    }

    adopt_conf_state(std::move(state));

    m_layout.set_shell_rows(dm_panels::BAR_H, m_panels->dock_y());
    refresh_backdrops(true, wallpaper);
    make_served_pictures();
    update_generation();

    m_damage.add_full();
    printf("stlxdm: config reloaded\r\n");
}

/* Moves `w` and every popup under it, since a popup's position is absolute */
void server::move_window_tree(dm_client& c, dm_window* w, int32_t dx, int32_t dy) {
    move_window(w, dx, dy);

    for (auto& child : c.windows) {
        if (child->parent_id == w->win_id) {
            move_window_tree(c, child.get(), dx, dy);
        }
    }
}

/* Keeps each toplevel's offset from its output's top-left. One whose output left moves to
 * the member holding the largest part of its frame, else to the main one. */
void server::move_windows_with_outputs(const layout& before) {
    uint32_t main = m_layout.main_output();
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->parent_id != 0) {
                continue;
            }

            const layout::place* was = before.find(w->output);
            const layout::place* now = m_layout.find(w->output);
            if (!now && was && w->current >= 0) {
                const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];
                damage_list::rect frame = decor::frame_rect(*w, w->x, w->y, static_cast<int32_t>(b.width),
                                                            static_cast<int32_t>(b.height));
                int32_t other = before.output_holding(frame, &m_layout);
                if (other >= 0) {
                    w->output = static_cast<uint32_t>(other);
                    was = before.find(w->output);
                    now = m_layout.find(w->output);
                }
            }

            if (!now) {
                w->output = main;
                now = m_layout.find(main);
            }

            int32_t dx = was ? now->rect.x - was->rect.x : 0;
            int32_t dy = was ? now->rect.y - was->rect.y : 0;
            w->placement.floating.x += dx;
            w->placement.floating.y += dy;
            move_window_tree(*c, w.get(), dx, dy);
        }
    }
}

/* Moves each toplevel whose grip the work areas leave uncovered, at its
 * position or one it was promised, into its output's work area */
void server::keep_windows_in_reach() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->parent_id != 0) {
                continue;
            }

            desktop_point corners[dm_window::SENT_CONF_MAX + 2];
            uint32_t count = 0;
            corners[count++] = { w->x, w->y };
            for (uint32_t i = 0; i < w->sent_conf_count; i++) {
                corners[count++] = { w->sent_confs[i].x, w->sent_confs[i].y };
            }

            if (w->target_w != 0) {
                corners[count++] = { w->target_x, w->target_y };
            }

            if (!m_layout.find(w->output)) {
                w->output = m_layout.main_output();
            }

            desktop_point move = m_layout.reach_move(w->output, corners, count, decor::TITLE_H);
            if (move.x != 0 || move.y != 0) {
                move_window_tree(*c, w.get(), move.x, move.y);
            }
        }
    }
}

desktop_point server::follow_outputs(desktop_point pointer, uint16_t buttons) {
    /* The switch shows the pointer on each recorded presenter, so first the records forget those the display dropped */
    if (composes_on_gpu() && m_display->refused_outputs() != 0) {
        sync_outputs();
        switch_to_cpu("an output has no GPU presenter");
    }

    layout before = m_layout;

    layout::member members[layout::MEMBERS_MAX];
    uint32_t count = gather_members(members);
    layout::arrangement stored;
    report_default(m_layout.rebuild(members, count, m_display->primary(), stored_layout(members, count, stored)));

    return adopt_layout(before, pointer, buttons);
}

/* Moves the outputs' records, the shell, windows and the pointer from `before` onto the layout, as
 * `follow_outputs()` describes */
desktop_point server::adopt_layout(const layout& before, desktop_point pointer, uint16_t buttons) {
    sync_outputs();

    dm_wallpaper wallpaper(*m_conf);

    /* The backdrop's size, not the last layout's, tells a failed rebuild apart, so the next event retries it */
    const damage_list::rect& main = m_layout.find(m_layout.main_output())->rect;
    const stlxgfx_surface_t* backdrop = m_gpu_backdrop ? m_gpu_backdrop->surface() : m_backdrop;
    bool backdrop_stale = static_cast<int32_t>(backdrop->width) != main.w ||
                          static_cast<int32_t>(backdrop->height) != main.h;
    bool rebuilt = false;
    if (m_layout.main_output() != before.main_output() || backdrop_stale) {
        dm_conf_state state;
        if (build_conf_state(state, wallpaper) == 0) {
            adopt_conf_state(std::move(state));
            rebuilt = true;
        } else {
            printf("stlxdm: desktop rebuild at %dx%d failed, keeping the running desktop\r\n", main.w, main.h);
        }
    }

    m_layout.set_shell_rows(dm_panels::BAR_H, m_panels->dock_y());
    refresh_backdrops(false, wallpaper);
    make_served_pictures();

    desktop_point moved = pointer;
    bool places_changed = !same_layout(before, m_layout);
    if (places_changed) {
        move_windows_with_outputs(before);
        moved = m_layout.follow_point(pointer, before);
        m_title_press_x += moved.x - pointer.x;
        m_title_press_y += moved.y - pointer.y;
    }

    /* A rebuild can move the dock's rows, which bound the main output's work area */
    if (places_changed || rebuilt) {
        keep_windows_in_reach();
        refit_placed_windows();
    }

    m_cursor_x = moved.x;
    m_cursor_y = moved.y;
    show_cursor();
    m_damage.add_full();
    update_generation();

    /* A clamp, a rebuild or a window's move comes from no device, so the
     * pointer routes as a motion for hover, its shape and the clients */
    route_pointer(moved.x, moved.y, buttons, 0, 0);

    return moved;
}

static const char* layout_outcome_text(uint32_t outcome) {
    switch (outcome) {
    case SWP_LAYOUT_APPLIED:
        return "applied";
    case SWP_LAYOUT_UNSAVED:
        return "applied, not stored";
    case SWP_LAYOUT_STALE:
        return "refused, built from another generation";
    case SWP_LAYOUT_INVALID:
        return "refused as invalid";
    case SWP_LAYOUT_OVERLAP:
        return "refused, two outputs overlap";
    default:
        return "refused, an output does not connect to the main output";
    }
}

desktop_point server::apply_layout_requests(desktop_point pointer, uint16_t buttons) {
    for (const dm_layout_request& queued : m_layout_requests) {
        layout before = m_layout;
        uint32_t outcome = apply_layout_request(queued.request);
        printf("stlxdm: layout asked by %s: %s\r\n", queued.client->app_id, layout_outcome_text(outcome));
        if (!same_layout(before, m_layout)) {
            pointer = adopt_layout(before, pointer, buttons);
        }

        dm_answer answer;
        answer.client = queued.client;
        answer.type = SWP_MSG_LAYOUT_RESULT;
        answer.layout.outcome = outcome;
        m_answers.push_back(answer);
    }

    m_layout_requests.clear();

    return pointer;
}

/* Checks `request` in the protocol's order against the members and, when it holds, applies it and stores it for
 * their monitors. Returns its SWP_LAYOUT_* outcome */
uint32_t server::apply_layout_request(const swp_set_layout& request) {
    if (request.generation != m_desktop.generation) {
        return SWP_LAYOUT_STALE;
    }

    /* While the layout has no members, it keeps places no request may name */
    layout::member members[layout::MEMBERS_MAX];
    if (gather_members(members) == 0 || request.count > SWP_OUTPUTS_MAX) {
        return SWP_LAYOUT_INVALID;
    }

    layout::arrangement asked;
    asked.main = request.main;
    asked.count = request.count;
    for (uint32_t i = 0; i < request.count; i++) {
        asked.entries[i] = { request.entries[i].output, request.entries[i].x, request.entries[i].y };
    }

    switch (m_layout.apply(asked)) {
    case layout::fault::invalid:
        return SWP_LAYOUT_INVALID;
    case layout::fault::overlap:
        return SWP_LAYOUT_OVERLAP;
    case layout::fault::apart:
        return SWP_LAYOUT_APART;
    case layout::fault::none:
        break;
    }

    m_store.remember(m_monitors, m_monitor_count, m_layout);
    int error = m_store.write(LAYOUTS_PATH);
    if (error != 0) {
        printf("stlxdm: %s could not be written (%s), keeping the layout for its monitors only until stlxdm exits\r\n",
               LAYOUTS_PATH, strerror(error));
        return SWP_LAYOUT_UNSAVED;
    }

    return SWP_LAYOUT_APPLIED;
}

static uint64_t sent_refresh(const swp_outputs& sent, uint32_t output) {
    for (uint32_t i = 0; i < sent.count; i++) {
        if (sent.outputs[i].output == output) {
            return sent.outputs[i].refresh_millihertz;
        }
    }

    return 0;
}

static const layout_store::identity* monitor_of(const layout_store::member* monitors, uint32_t count,
                                                uint32_t output) {
    for (uint32_t i = 0; i < count; i++) {
        if (monitors[i].output == output) {
            return &monitors[i].monitor;
        }
    }

    return nullptr;
}

/* Fills `out`'s members from the layout and the monitors it was last built for, which no output going dark changes,
 * an output that is not served keeping the refresh it was last sent with */
void server::describe_outputs(swp_outputs& out) const {
    out.count = m_layout.count();
    for (uint32_t i = 0; i < m_layout.count(); i++) {
        const layout::place& place = m_layout.at(i);
        const presenter* pres = m_display->presenter_of(place.output);
        const layout_store::identity* monitor = monitor_of(m_monitors, m_monitor_count, place.output);
        damage_list::rect work = m_layout.work_area(place.output);
        swp_desktop_output& entry = out.outputs[i];

        entry.output = place.output;
        entry.flags = place.output == m_layout.main_output() ? SWP_DESKTOP_MAIN : 0;
        entry.x = place.rect.x;
        entry.y = place.rect.y;
        entry.width = static_cast<uint32_t>(place.rect.w);
        entry.height = static_cast<uint32_t>(place.rect.h);

        entry.refresh_millihertz = pres ? pres->refresh_millihertz() : sent_refresh(m_desktop, place.output);
        entry.work = { work.x, work.y, work.w, work.h };

        if (monitor) {
            const char* gap = monitor->maker[0] != '\0' && monitor->name[0] != '\0' ? " " : "";
            snprintf(entry.monitor, sizeof(entry.monitor), "%.3s%s%.15s", monitor->maker, gap, monitor->name);
        }
    }
}

void server::update_generation() {
    swp_outputs next = {};
    next.generation = m_desktop.generation;
    describe_outputs(next);
    if (memcmp(&next, &m_desktop, sizeof(next)) != 0) {
        next.generation++;
        m_desktop = next;
    }
}

void server::flush_outputs() {
    for (auto& c : m_clients) {
        if (c->hello_done && c->outputs_generation < m_desktop.generation) {
            send_to(*c, SWP_MSG_OUTPUTS, &m_desktop, sizeof(m_desktop));
            c->outputs_generation = m_desktop.generation;
        }
    }

    flush_window_outputs();

    for (dm_answer& answer : m_answers) {
        if (answer.type == SWP_MSG_LAYOUT_RESULT) {
            answer.layout.generation = m_desktop.generation;
            send_to(*answer.client, answer.type, &answer.layout, sizeof(answer.layout));
        } else {
            send_to(*answer.client, answer.type, &answer.mode, sizeof(answer.mode));
        }
    }

    m_answers.clear();
}

void server::flush_window_outputs() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            if (w->parent_id != 0 || !w->mapped) {
                continue;
            }

            bool output_changed = !w->output_sent || w->sent_output != w->output;
            bool moved = w->sent_x != w->x || w->sent_y != w->y;
            bool held = w.get() == m_drag || w.get() == m_resize;
            if (!output_changed && (!moved || held)) {
                continue;
            }

            swp_window_output msg = { w->win_id, w->output, w->x, w->y };
            send_to(*c, SWP_MSG_WINDOW_OUTPUT, &msg, sizeof(msg));

            w->output_sent = true;
            w->sent_output = w->output;
            w->sent_x = w->x;
            w->sent_y = w->y;
        }
    }
}

void server::shutdown() {
    for (auto& c : m_clients) {
        close(c->fd);
    }
    m_clients.clear();

    /* Panels hold views into the backdrop, so they go first */
    if (m_panels) {
        m_panels->shutdown();
    }
    stlxgfx_destroy_surface(m_backdrop);
    m_backdrop = nullptr;
    m_gpu_backdrop.reset();

    stlxgfx_destroy_surface(m_frost);
    m_frost = nullptr;
    m_gpu_frost.reset();

    for (dm_output& out : m_outputs) {
        stlxgfx_destroy_surface(out.backdrop);
        stlxgfx_destroy_surface(out.frost);
        out = {};
    }

    if (m_power) {
        m_power->shutdown();
    }

    m_gpu_capture.reset();

    if (m_listen_fd >= 0) {
        close(m_listen_fd);
        unlink(SWP_SOCKET_PATH);
        m_listen_fd = -1;
    }
}

void server::collect_fds(std::vector<pollfd>& fds) const {
    fds.push_back({ m_listen_fd, POLLIN, 0 });
    for (const auto& c : m_clients) {
        short events = POLLIN;
        if (!c->out_q.empty()) {
            events |= POLLOUT;
        }
        fds.push_back({ c->fd, events, 0 });
    }
}

void server::pump(const std::vector<pollfd>& fds) {
    /* Slot 0 is the listen socket, clients follow in collect_fds order */
    if (fds[0].revents & POLLIN) {
        accept_one();
    }

    for (size_t i = 0; i < m_clients.size() && i + 1 < fds.size(); i++) {
        if (fds[i + 1].revents & POLLOUT) {
            flush_client(*m_clients[i]);
        }
        if (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
            pump_client(*m_clients[i]);
        }
    }

    for (size_t i = m_clients.size(); i-- > 0;) {
        if (m_clients[i]->dead) {
            drop_client(*m_clients[i]);
            m_clients.erase(m_clients.begin() + static_cast<long>(i));
        }
    }
}

void server::accept_one() {
    int fd = accept(m_listen_fd, nullptr, nullptr);
    if (fd < 0) {
        return;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    m_clients.push_back(std::make_unique<dm_client>(fd));
}

void server::pump_client(dm_client& c) {
    while (true) {
        iovec iov = { c.rd_buf + c.rd_have, sizeof(c.rd_buf) - c.rd_have };
        alignas(cmsghdr) uint8_t control[HANDLE_CONTROL_BYTES];

        msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        ssize_t n = recvmsg(c.fd, &msg, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            c.dead = true;
            return;
        }
        if (n == 0) {
            c.dead = true;
            return;
        }

        if (!queue_received_handles(c, msg)) {
            c.dead = true;
            return;
        }

        c.rd_have += static_cast<uint32_t>(n);

        uint32_t off = 0;
        while (c.rd_have - off >= sizeof(swp_header)) {
            swp_header hdr;
            memcpy(&hdr, c.rd_buf + off, sizeof(hdr));
            if (hdr.length > SWP_MAX_MSG_SIZE - sizeof(hdr)) {
                c.dead = true;
                return;
            }

            if (c.rd_have - off < sizeof(hdr) + hdr.length) {
                break;
            }

            handle_message(c, hdr, c.rd_buf + off + sizeof(hdr));
            if (c.dead) {
                return;
            }
            off += sizeof(hdr) + hdr.length;
        }

        if (off > 0) {
            memmove(c.rd_buf, c.rd_buf + off, c.rd_have - off);
            c.rd_have -= off;
        }

        /* Handles sent with any message but an attach, or more than an
         * attach takes, are left over here */
        if (c.queued_handle_count > 0 && !may_begin_gpu_attach(c)) {
            c.dead = true;
            return;
        }
    }
}

void server::handle_message(dm_client& c, const swp_header& hdr,
                            const uint8_t* payload) {
    /* The hello handshake gates everything else */
    if (!c.hello_done) {
        if (hdr.type != SWP_MSG_HELLO || hdr.length != sizeof(swp_hello)) {
            c.dead = true;
            return;
        }

        const swp_hello* m = reinterpret_cast<const swp_hello*>(payload);
        if (m->proto_version != SWP_VERSION) {
            c.dead = true;
            return;
        }

        memcpy(c.app_id, m->app_id, sizeof(c.app_id));
        c.app_id[sizeof(c.app_id) - 1] = '\0';
        c.hello_done = true;
        c.may_attach_gpu = (m_gpu_info.flags & SWP_GPU_ACCEPTS) != 0;

        const damage_list::rect& main = m_layout.find(m_layout.main_output())->rect;
        swp_hello_reply reply = { SWP_VERSION, SWP_FMT_XRGB8888,
                                  static_cast<uint32_t>(main.w),
                                  static_cast<uint32_t>(main.h) };
        uint16_t flags = SWP_HELLO_GPU_INFO | SWP_HELLO_OUTPUTS;
        if (!send_to(c, SWP_MSG_HELLO_REPLY, &reply, sizeof(reply), flags) ||
            !send_to(c, SWP_MSG_GPU_INFO, &m_gpu_info, sizeof(m_gpu_info)) ||
            !send_to(c, SWP_MSG_OUTPUTS, &m_desktop, sizeof(m_desktop))) {
            c.dead = true;
        }

        c.outputs_generation = m_desktop.generation;
        return;
    }

    switch (hdr.type) {
    case SWP_MSG_CREATE_WINDOW:
        if (hdr.length == sizeof(swp_create_window)) {
            handle_create_window(c, payload);
            return;
        }
        break;
    case SWP_MSG_DESTROY_WINDOW:
        if (hdr.length == sizeof(swp_destroy_window)) {
            handle_destroy_window(c, payload);
            return;
        }
        break;
    case SWP_MSG_SET_WINDOW:
        if (hdr.length == sizeof(swp_set_window)) {
            handle_set_window(c, payload);
            return;
        }
        break;
    case SWP_MSG_ATTACH_BUFFER:
        if (hdr.length == sizeof(swp_attach_buffer)) {
            handle_attach_buffer(c, payload);
            return;
        }
        break;
    case SWP_MSG_DETACH_BUFFER:
        if (hdr.length == sizeof(swp_detach_buffer)) {
            handle_detach_buffer(c, payload);
            return;
        }
        break;
    case SWP_MSG_COMMIT:
        if (hdr.length == sizeof(swp_commit)) {
            handle_commit(c, payload);
            return;
        }
        break;
    case SWP_MSG_CLIPBOARD_SET: {
        if (hdr.length < sizeof(swp_clipboard_set)) {
            break;
        }

        const auto* m = reinterpret_cast<const swp_clipboard_set*>(payload);
        if (m->len > SWP_CLIPBOARD_MAX ||
            hdr.length != sizeof(*m) + m->len) {
            break;
        }

        const char* text = reinterpret_cast<const char*>(payload + sizeof(*m));
        m_clipboard.assign(text, text + m->len);
        return;
    }
    case SWP_MSG_CLIPBOARD_GET: {
        uint8_t reply[sizeof(swp_clipboard_data) + SWP_CLIPBOARD_MAX];
        swp_clipboard_data prefix = { static_cast<uint32_t>(m_clipboard.size()) };
        memcpy(reply, &prefix, sizeof(prefix));
        if (!m_clipboard.empty()) {
            memcpy(reply + sizeof(prefix), m_clipboard.data(),
                   m_clipboard.size());
        }

        send_to(c, SWP_MSG_CLIPBOARD_DATA, reply,
                static_cast<uint32_t>(sizeof(prefix) + m_clipboard.size()));
        return;
    }
    case SWP_MSG_CAPTURE:
        /* Deferred until the screenshot consumer exists, the wire needs
         * a completion message first */
        return;
    case SWP_MSG_GET_MODES:
        if (hdr.length != sizeof(swp_get_modes)) {
            break;
        }

        handle_get_modes(c, payload);
        return;
    case SWP_MSG_SET_MODE:
        if (hdr.length != sizeof(swp_set_mode)) {
            break;
        }

        queue_mode_message(c, hdr.type, payload);
        return;
    case SWP_MSG_KEEP_MODE:
        if (hdr.length != sizeof(swp_keep_mode)) {
            break;
        }

        queue_mode_message(c, hdr.type, payload);
        return;
    case SWP_MSG_ATTACH_GPU_BUFFER:
        if (hdr.length != sizeof(swp_attach_gpu_buffer)) {
            break;
        }

        handle_attach_gpu_buffer(c, payload);
        return;
    case SWP_MSG_COMMIT_GPU:
        if (hdr.length != sizeof(swp_commit_gpu)) {
            break;
        }

        handle_commit_gpu(c, payload);
        return;
    case SWP_MSG_SET_LAYOUT: {
        if (hdr.length != sizeof(swp_set_layout)) {
            break;
        }

        dm_layout_request queued;
        queued.client = &c;
        memcpy(&queued.request, payload, sizeof(queued.request));
        m_layout_requests.push_back(queued);
        return;
    }
    default:
        break;
    }

    c.dead = true;
}

void server::drop_client(dm_client& c) {
    forget_mode_client(c);

    /* The client is freed once this returns, so no queued request or staged answer may name it */
    auto names_client = [&c](const auto& entry) { return entry.client == &c; };
    std::erase_if(m_layout_requests, names_client);
    std::erase_if(m_answers, names_client);

    while (!c.windows.empty()) {
        destroy_window_tree(c, c.windows.back()->win_id);
    }

    for (const auto& d : c.dismissed_buffers) {
        close(d.buffer.release);
    }

    c.dismissed_buffers.clear();

    for (uint32_t i = 0; i < c.queued_handle_count; i++) {
        close(c.queued_handles[i]);
    }

    c.queued_handle_count = 0;

    close(c.fd);
    c.fd = -1;
}

void server::spawn_shortcut(const dm_hotkey& hotkey) {
    spawn_app(hotkey.path, hotkey.args);
}

constexpr size_t OUT_Q_LIMIT = 64 * 1024;

/* Nonblocking send that queues whatever the socket refuses. Overflow
 * means the client stopped reading long ago and it is disconnected. */
bool server::send_to(dm_client& c, uint16_t type,
                     const void* payload, uint32_t length,
                     uint16_t header_flags) {
    if (c.dead) {
        return false;
    }

    swp_header hdr = { type, header_flags, length };
    uint8_t msg[SWP_MAX_MSG_SIZE];
    memcpy(msg, &hdr, sizeof(hdr));
    if (length > 0) {
        memcpy(msg + sizeof(hdr), payload, length);
    }

    size_t total = sizeof(hdr) + length;
    size_t sent = 0;

    /* Queued bytes must go first to preserve message order */
    if (c.out_q.empty()) {
        ssize_t n = send(c.fd, msg, total, MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(total)) {
            return true;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c.dead = true;
            return false;
        }

        sent = n > 0 ? static_cast<size_t>(n) : 0;
    }

    if (c.out_q.size() + (total - sent) > OUT_Q_LIMIT) {
        c.dead = true;
        return false;
    }

    /* Any append that is not a lone motion invalidates the coalesce
     * marker so ordering is never disturbed */
    c.q_motion_off = SIZE_MAX;
    c.out_q.insert(c.out_q.end(), msg + sent, msg + total);
    return true;
}

/* Flush one window's staged records as a single EVENT message. Under
 * queue pressure the batch keeps only its newest motion, and a lone
 * motion overwrites the queued one in place, so a stalled client sees
 * the latest position without the queue growing. */
void server::flush_window_events(dm_client& c, dm_window& w) {
    if (w.ev_batch_count == 0 || c.dead) {
        w.ev_batch_count = 0;
        return;
    }

    if (!c.out_q.empty()) {
        int32_t last_motion = -1;
        for (uint32_t i = 0; i < w.ev_batch_count; i++) {
            if (w.ev_batch[i].kind == SWP_EV_MOTION) {
                last_motion = static_cast<int32_t>(i);
            }
        }

        uint32_t keep = 0;
        for (uint32_t i = 0; i < w.ev_batch_count; i++) {
            if (w.ev_batch[i].kind == SWP_EV_MOTION &&
                static_cast<int32_t>(i) != last_motion) {
                continue;
            }
            w.ev_batch[keep++] = w.ev_batch[i];
        }
        w.ev_batch_count = keep;

        if (w.ev_batch_count == 1 &&
            w.ev_batch[0].kind == SWP_EV_MOTION &&
            c.q_motion_off != SIZE_MAX && c.q_motion_win == w.win_id) {
            memcpy(c.out_q.data() + c.q_motion_off +
                       sizeof(swp_header) + sizeof(swp_event_prefix),
                   &w.ev_batch[0], sizeof(swp_event_rec));
            w.ev_batch_count = 0;
            return;
        }
    }

    struct {
        swp_event_prefix prefix;
        swp_event_rec recs[DM_EV_BATCH_MAX];
    } msg;
    msg.prefix = { w.win_id, w.ev_batch_count };
    memcpy(msg.recs, w.ev_batch,
           w.ev_batch_count * sizeof(swp_event_rec));
    uint32_t length = static_cast<uint32_t>(
        sizeof(swp_event_prefix) +
        w.ev_batch_count * sizeof(swp_event_rec));

    size_t off = c.out_q.size();
    bool lone_motion = w.ev_batch_count == 1 &&
                       w.ev_batch[0].kind == SWP_EV_MOTION;
    w.ev_batch_count = 0;
    if (!send_to(c, SWP_MSG_EVENT, &msg, length)) {
        return;
    }

    /* The coalesce marker is valid only when a whole lone-motion
     * message sits in the queue */
    if (lone_motion && c.out_q.size() > off &&
        c.out_q.size() - off == sizeof(swp_header) + length) {
        c.q_motion_off = off;
        c.q_motion_win = w.win_id;
    }
}

/* Cross-window delivery order within one wakeup is unspecified, only
 * per-window order is promised. */
void server::flush_events() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            flush_window_events(*c, *w);
        }
    }
}

void server::flush_client(dm_client& c) {
    if (c.dead || c.out_q.empty()) {
        return;
    }

    ssize_t n = send(c.fd, c.out_q.data(), c.out_q.size(), MSG_NOSIGNAL);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            c.dead = true;
        }
        return;
    }

    c.out_q.erase(c.out_q.begin(), c.out_q.begin() + n);

    if (c.q_motion_off != SIZE_MAX) {
        if (static_cast<size_t>(n) > c.q_motion_off) {
            c.q_motion_off = SIZE_MAX;
        } else {
            c.q_motion_off -= static_cast<size_t>(n);
        }
    }
}

