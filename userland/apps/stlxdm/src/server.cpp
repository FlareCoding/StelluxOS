#include "server.hpp"
#include "material.hpp"
#include "offer_intake.hpp"
#include "shared_object.hpp"

#include "shell/chord.hpp"
#include "shell/keycap.hpp"

#include <stlx/gpu_abi.h>
#include <stlx/handle.h>
#include <stlx/proc.h>
#include <stlxgfx/image.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

/* Room for the most handles one message carries. The kernel fits `(controllen - sizeof(cmsghdr)) / 4`, so CMSG_SPACE's
 * padding would fit one more. */
constexpr size_t HANDLE_CONTROL_BYTES = CMSG_LEN(SWP_HANDLES_MAX * sizeof(int));

/* CLIPBOARD_GETs one client may leave unanswered, which only a client
 * that asks without reading its socket reaches */
constexpr size_t CLIPBOARD_ASKS_MAX = 64;

/* A retired GPU compositor's timeline is read at least this often */
constexpr int64_t RETIRED_QUERY_INTERVAL_NS = 100000000;

/* The sources the clipboard history names for stlxdm's own copies, from Quick Actions and from captures */
constexpr const char* QUICK_ACTIONS_LABEL = "Quick Actions";
constexpr const char* SCREENSHOT_LABEL = "Screenshot";

constexpr const char* PNG_TYPE = "image/png";

/* As many captures may wait to encode as there are cards, each holding a whole output's pixels */
constexpr size_t CAPTURES_WAITING_MAX = 3;

/* The arguments of its own a file opener's drop-in may give before the paths */
constexpr size_t OPENER_ARGS_MAX = 16;

static int64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

/* A client's received handles as offers take them */
static handle_queue handles_of(dm_client& c) {
    return { c.queued_handles, dm_client::QUEUED_HANDLES_MAX, &c.queued_handle_count };
}

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

/* Starts `path` with exactly `args`, so a file's path holding spaces stays one argument */
static bool exec_app(const char* path, const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }

    argv.push_back(nullptr);
    int handle = proc_exec(path, argv.data());
    if (handle < 0) {
        printf("stlxdm: failed to spawn %s\r\n", path);
        return false;
    }

    proc_detach(handle);
    printf("stlxdm: spawned %s\r\n", path);

    return true;
}

/* Opens files dropped on a launcher in its program, after the program's own arguments for their type and each path its
 * own argument, or the terminal in a dropped folder */
bool server::open_drop_in_launcher(const stlxconf_pin_t& pin, const std::vector<dock_file>& files) {
    if (files.empty()) {
        return false;
    }

    if (is_terminal_program(pin.path) && files.size() == 1 && files.front().is_folder) {
        return exec_app(pin.path, { "--cwd", files.front().path });
    }

    char own[sizeof(stlxconf_file_handler_t::args)] = "";
    strncpy(own, files.front().opener_args.c_str(), sizeof(own) - 1);
    const char* own_argv[OPENER_ARGS_MAX];
    int own_argc = stlxconf_split_args(own, own_argv, static_cast<int>(OPENER_ARGS_MAX));

    std::vector<std::string> args(own_argv, own_argv + own_argc);
    for (const dock_file& file : files) {
        args.push_back(file.path);
    }

    return exec_app(pin.path, args);
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

    /* The clipboard history begins with stlxdm, at the bar's clock */
    time_t started = time(nullptr);
    char since[8] = "";
    strftime(since, sizeof(since), "%H:%M", gmtime(&started));
    m_clipboard_since = since;

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

    /* An animation draws its frame again only once dropped, and the GPU cannot read a frame drawn in the heap */
    for (window_animation& a : m_animations) {
        a.window->frame = frame_picture();
    }

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

    if (!m_capture_encoder.start()) {
        printf("stlxdm: captures stay off the clipboard, since their encoder could not start\r\n");
    }

    frost_lookup frost_of_output = [this](uint32_t output) {
        return m_outputs[output].pres ? frost_of(m_outputs[output]) : nullptr;
    };
    m_thumbnails.set_frost_lookup(frost_of_output);
    m_capture.set_frost_lookup(frost_of_output);

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
        open_power();
    };

    out.panels->on_copy = [this](const std::string& text) {
        copy_text(text);
    };

    out.panels->on_copy_file = [this](const std::string& path) {
        copy_file(path);
    };

    out.panels->use_clipboard(&m_history, m_clipboard_since);
    out.panels->on_clipboard_paste = [this](uint64_t id) {
        choose_clipboard_entry(id, true);
    };

    out.panels->on_clipboard_copy = [this](uint64_t id) {
        choose_clipboard_entry(id, false);
    };

    out.panels->on_clipboard_pin = [this](uint64_t id, bool pinned) {
        m_history.set_pinned(id, pinned);
        note_history_change();
    };

    /* Removing entries changes the current item only when one of them was current, which leaves the clipboard empty */
    out.panels->on_clipboard_remove = [this](uint64_t id) {
        bool had_current = !m_history.current().empty();
        m_history.remove(id);
        if (had_current && m_history.current().empty()) {
            note_clipboard_change();
        } else {
            note_history_change();
        }
    };

    out.panels->on_clipboard_clear = [this]() {
        bool had_current = !m_history.current().empty();
        m_history.clear_unpinned();
        if (had_current && m_history.current().empty()) {
            note_clipboard_change();
        } else {
            note_history_change();
        }
    };

    out.panels->on_exec = [](const char* path, const std::vector<std::string>& args) {
        return exec_app(path, args);
    };

    out.panels->use_picture_maker(m_gpu_composes ? m_gpu_compositor.get() : nullptr);

    /* Shortcuts with parseable chords become live hotkeys, each running a program or one of stlxdm's own actions */
    for (uint32_t i = 0; i < m_conf->shortcut_count; i++) {
        const stlxconf_shortcut_t& sc = m_conf->shortcuts[i];
        stlxconf_action_kind_t kind = stlxconf_action_kind(sc.action);
        bool runs_program = kind == STLXCONF_ACTION_EXEC && sc.path[0];
        bool runs_own_action = kind == STLXCONF_ACTION_POWER || kind == STLXCONF_ACTION_QUICK_ACTIONS ||
                               kind == STLXCONF_ACTION_PASTE || kind == STLXCONF_ACTION_CLIPBOARD ||
                               kind == STLXCONF_ACTION_SCREENSHOT;
        if (!runs_program && !runs_own_action) {
            continue;
        }

        dm_hotkey hk;
        if (parse_chord(sc.key, &hk.mods, &hk.usage)) {
            hk.kind = kind;
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
    m_history.set_limits(m_conf->clipboard_history_items, static_cast<uint64_t>(m_conf->clipboard_history_mb) << 20);
    note_history_change();

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
    bool animates = !m_animations.empty() || drag_moves();
    return !out.damage.empty() || has_latch_work(output) || has_configure_work() || shell_pending || animates;
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
    /* Frozen frames no longer match outputs that changed, so a capture ends, the repaint below showing the desktop */
    if (m_capture.active()) {
        printf("stlxdm: the outputs changed during a capture, which ends\r\n");
        m_capture.close();
    }

    /* Thumbnails sit at corners that moved, and their captures stay on the clipboard */
    m_thumbnails.clear(m_damage);
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
    m_history.clear();

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
        if (!c->out_q.empty() || !c->clipboard_asks.empty() || !c->drop_asks.empty()) {
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
            catch_up_watcher(*m_clients[i]);
            send_clipboard_answers(*m_clients[i]);
            send_drop_answers(*m_clients[i]);
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

        if (!queue_received_handles(handles_of(c), msg)) {
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

        /* Handles sent with a message that takes none, or more than a
         * message takes, are left over here */
        if (c.queued_handle_count > 0 && !may_begin_handle_message(c.rd_buf, c.rd_have)) {
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
    case SWP_MSG_CLIPBOARD_SET:
        if (hdr.length != sizeof(swp_clipboard_set)) {
            break;
        }

        set_clipboard(c, payload);
        return;
    case SWP_MSG_CLIPBOARD_OFFER:
        if (hdr.length != sizeof(swp_offer)) {
            break;
        }

        offer_clipboard(c, payload);
        return;
    case SWP_MSG_CLIPBOARD_GET:
        if (hdr.length != 0 && hdr.length != sizeof(swp_clipboard_get)) {
            break;
        }

        ask_clipboard(c, hdr, payload);
        return;
    case SWP_MSG_CLIPBOARD_WATCH:
        if (hdr.length != 0) {
            break;
        }

        c.watches_clipboard = true;
        send_clipboard_types(c);
        return;
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
    case SWP_MSG_ACTIVATE_WINDOW:
        if (hdr.length != sizeof(swp_activate_window)) {
            break;
        }

        handle_activate_window(c, payload);
        return;
    case SWP_MSG_START_DRAG:
        if (hdr.length != sizeof(swp_start_drag)) {
            break;
        }

        handle_start_drag(c, payload);
        return;
    case SWP_MSG_CANCEL_DRAG:
        if (hdr.length != sizeof(swp_cancel_drag)) {
            break;
        }

        handle_cancel_drag(c, payload);
        return;
    case SWP_MSG_DROP_STATUS:
        if (hdr.length != sizeof(swp_drop_status)) {
            break;
        }

        handle_drop_status(c, payload);
        return;
    case SWP_MSG_DROP_GET:
        if (hdr.length != sizeof(swp_drop_get)) {
            break;
        }

        handle_drop_get(c, payload);
        return;
    case SWP_MSG_DROP_FINISH:
        if (hdr.length != sizeof(swp_drop_finish)) {
            break;
        }

        handle_drop_finish(c, payload);
        return;
    default:
        break;
    }

    c.dead = true;
}

void server::drop_client(dm_client& c) {
    forget_mode_client(c);
    forget_drag_client(c);

    /* The client is freed once this returns, so no queued request, staged answer or owned copy may name it */
    auto names_client = [&c](const auto& entry) { return entry.client == &c; };
    std::erase_if(m_layout_requests, names_client);
    std::erase_if(m_answers, names_client);
    if (m_clipboard_owner == &c) {
        m_clipboard_owner = nullptr;
    }

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

/* A program started while a popover holds the keyboard closes the popover, so typing reaches the program's window */
void server::run_shortcut(const dm_hotkey& hotkey) {
    if (hotkey.kind == STLXCONF_ACTION_POWER) {
        open_power();
        return;
    }

    if (hotkey.kind == STLXCONF_ACTION_QUICK_ACTIONS) {
        m_panels->toggle_quick_actions();
        return;
    }

    if (hotkey.kind == STLXCONF_ACTION_CLIPBOARD) {
        m_panels->toggle_clipboard();
        return;
    }

    /* A capture keeps an open palette in the frame */
    if (hotkey.kind == STLXCONF_ACTION_SCREENSHOT) {
        take_screenshot(hotkey.args);
        return;
    }

    if (m_panels->popover_takes_keys()) {
        m_panels->close_popover();
    }

    if (hotkey.kind == STLXCONF_ACTION_PASTE) {
        paste_into_focus();
        return;
    }

    spawn_app(hotkey.path, hotkey.args);
}

/* The focused window pastes the clipboard as its own paste key would, whatever key that is, after the types its
 * client still lacks, which go even behind a full queue since the paste reads them */
void server::paste_into_focus() {
    if (!m_focus) {
        return;
    }

    dm_client* c = window_owner(m_focus);
    if (c && c->watches_clipboard && c->clipboard_generation != m_clipboard_generation) {
        send_clipboard_types(*c);
    }

    swp_event_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = SWP_EV_PASTE;
    send_event(m_focus, rec);
}

/* The overlay owns every input while open, so a popover beneath it closes first */
void server::open_power() {
    m_panels->close_popover_now();
    m_power->open();
    m_damage.add_full();
}

void server::take_screenshot(const char* mode) {
    if (strcmp(mode, "screen") == 0) {
        capture_screen();
        return;
    }

    if (strcmp(mode, "region") == 0) {
        open_capture();
        return;
    }

    if (strcmp(mode, "window") == 0) {
        capture_focused_window();
        return;
    }

    printf("stlxdm: a screenshot shortcut names no capture stlxdm takes: '%s'\r\n", mode);
}

/* Every served output freezes before anything else draws, so menus and hover states hold still while a region or a
 * window is aimed */
void server::open_capture() {
    if (m_capture.active() || m_power->active()) {
        return;
    }

    std::vector<frozen_frame> frames(gfxfb::GFXFB_OUTPUTS_MAX);
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        if (m_outputs[output].pres && !freeze_output(m_outputs[output], frames[output])) {
            printf("stlxdm: freezing output %u for a capture failed\r\n", output);
            return;
        }
    }

    /* The shown toplevels, top first, which window mode lights under the pointer */
    std::vector<capture_window> windows;
    for (size_t i = m_zorder.size(); i-- > 0;) {
        dm_window* w = m_zorder[i];
        if (w->parent_id != 0 || w->current < 0 || is_hidden(*w)) {
            continue;
        }

        const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];
        int32_t bw = static_cast<int32_t>(b.width);
        int32_t bh = static_cast<int32_t>(b.height);
        bool is_decorated = decor::decorated(*w);
        damage_list::rect frame = is_decorated ? decor::frame_rect(*w, w->x, w->y, bw, bh)
                                               : damage_list::rect{ w->x, w->y, bw, bh };

        damage_list::rect bounds = is_decorated ? decor::bounds(*w, chrome_of(*w).focused) : frame;
        windows.push_back({ w, frame, is_decorated ? decor::CORNER_R : 0, w->title, bounds });
    }

    m_capture.open(std::move(frames), std::move(windows), m_cursor_x, m_cursor_y);
    move_cursor(m_cursor_x, m_cursor_y, m_capture.cursor_shape());
    m_damage.add_full();
}

/* Alt Print takes the focused window at once, alone with its shadow */
void server::capture_focused_window() {
    if (!m_focus || m_capture.active() || m_power->active()) {
        return;
    }

    dm_client* owner = window_owner(m_focus);
    const dm_window* top = owner ? &toplevel_of(*owner, *m_focus) : nullptr;
    for (dm_window* w : m_zorder) {
        if (w == top && w->current >= 0) {
            capture_window_alone(*w, true);
        }
    }
}

void server::capture_window_alone(dm_window& w, bool has_shadow) {
    stlxgfx_surface_t* pixels = compose_window_alone(w, has_shadow);
    if (!pixels) {
        printf("stlxdm: composing a window alone for a capture failed\r\n");
        return;
    }

    encode_capture(pixels, true, capture_kind::window, w.output);
}

void server::close_capture() {
    m_capture.close();
    move_cursor(m_cursor_x, m_cursor_y, SWP_CURSOR_ARROW);
    m_damage.add_full();
}

/* A region's frozen pixels go to the encoder, or a clicked window composed alone if it is still shown, and the live
 * desktop comes back */
void server::capture_region(const capture_target& target) {
    if (target.window) {
        close_capture();
        for (dm_window* w : m_zorder) {
            if (w == target.window && w->current >= 0) {
                capture_window_alone(*w, target.has_shadow);
            }
        }

        return;
    }

    stlxgfx_surface_t* pixels = copy_frozen_rect(m_capture.frame(target.output), target.rect);
    close_capture();
    if (!pixels) {
        printf("stlxdm: copying a %d by %d region for a capture failed\r\n", target.rect.w, target.rect.h);
        return;
    }

    encode_capture(pixels, false, capture_kind::region, target.output);
}

/* The output under the pointer goes to the encoder at once, composed whole as its next frame shows it */
void server::capture_screen() {
    int32_t under = m_layout.output_at(m_cursor_x, m_cursor_y);
    uint32_t output = under >= 0 ? static_cast<uint32_t>(under) : m_layout.main_output();
    dm_output& out = m_outputs[output];
    if (!out.pres) {
        return;
    }

    frozen_frame frozen;
    stlxgfx_surface_t* pixels = nullptr;
    if (freeze_output(out, frozen)) {
        damage_list::rect whole = { 0, 0, static_cast<int32_t>(out.pres->width()),
                                    static_cast<int32_t>(out.pres->height()) };
        pixels = copy_frozen_rect(frozen, whole);
    }

    if (!pixels) {
        printf("stlxdm: capturing output %u failed\r\n", output);
        return;
    }

    encode_capture(pixels, false, capture_kind::screen, output);
}

/* A capture's thumbnail shows at once, shrunk from its pixels before the encoder takes them, and goes again when no
 * encoder runs to take them. While as many captures wait as the cards show, a new one is refused rather than an older
 * one dropped, so a burst parks no more pixels than the cards show and every card it showed reaches the clipboard. */
void server::encode_capture(stlxgfx_surface_t* pixels, bool keeps_alpha, capture_kind kind, uint32_t output) {
    if (m_capture_encoder.waiting() >= CAPTURES_WAITING_MAX) {
        printf("stlxdm: %zu captures still wait to encode, so a new one is not taken\r\n", CAPTURES_WAITING_MAX);
        stlxgfx_destroy_surface(pixels);
        return;
    }

    uint64_t job = m_next_capture++;
    uint64_t now = static_cast<uint64_t>(monotonic_ns());
    m_thumbnails.add(job, kind, pixels, keeps_alpha, output, m_outputs[output].place, now, m_damage);
    if (!m_capture_encoder.encode({ job, pixels, keeps_alpha })) {
        m_thumbnails.remove(job, m_damage);
    }
}

/* An encoded capture becomes the clipboard as a copy does, the copy it replaces staying in the history, which its
 * thumbnail's Discard puts back. A capture discarded while it encoded stays off. */
void server::pump_captures() {
    capture_png png;
    while (m_capture_encoder.take(&png)) {
        stlxgfx_destroy_surface(png.pixels);
        bool is_dropped = m_thumbnails.take_dropped(png.id);
        if (png.object < 0 || is_dropped) {
            if (png.object < 0) {
                printf("stlxdm: keeping capture %llu on the clipboard failed, errno %d\r\n",
                       static_cast<unsigned long long>(png.id), png.error);
            } else {
                close(png.object);
            }

            m_thumbnails.remove(png.id, m_damage);
            continue;
        }

        replace_clipboard(clipboard_item({ { PNG_TYPE, png.len, png.object } }, 0), SCREENSHOT_LABEL);
        const char* kind = m_thumbnails.kind_name_of(png.id);
        m_history.set_name(m_history.current_id(), kind ? kind : "");
        m_thumbnails.encoded(png.id, m_history.current_id());
    }
}

/* Discard takes the capture off the clipboard and out of the history, putting back the copy it replaced, which
 * changes the current item only when the capture was current. A capture still waiting to encode is dropped. */
void server::discard_capture(const thumbnail_click& click) {
    m_thumbnails.remove(click.job, m_damage);
    if (click.entry == 0) {
        if (m_capture_encoder.drop(click.job)) {
            m_thumbnails.take_dropped(click.job);
        }

        return;
    }

    bool was_current = m_history.is_current(click.entry);
    m_history.discard(click.entry);
    if (was_current) {
        note_clipboard_change();
    } else {
        note_history_change();
    }
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
 * per-window order is promised. The clipboard's types leave first, and a
 * paste sends its window's client the types it still lacks. */
void server::flush_events() {
    flush_clipboard();
    flush_drag();

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

/* Every copy comes through here, so the history keeps it and each watching client hears of it */
void server::replace_clipboard(clipboard_item item, std::string label) {
    m_history.copy(std::move(item), std::move(label), monotonic_ns());
    note_clipboard_change();
}

/* The current item changed: each watcher hears of it at the next flush, however many changes come first, and the
 * item is no client's own copy unless its copier says so after */
void server::note_clipboard_change() {
    m_clipboard_generation++;
    m_history_changed = true;
    m_clipboard_owner = nullptr;
}

/* Only the history's other entries changed, which its views show and watchers never hear of */
void server::note_history_change() {
    m_history_changed = true;
}

/* Each watcher behind the clipboard's generation hears it once, and the history's views and the capture captions
 * refresh once, so a client copying in a loop costs every watcher one message per wakeup instead of one per copy */
void server::flush_clipboard() {
    for (auto& c : m_clients) {
        catch_up_watcher(*c);
    }

    if (m_history_changed) {
        if (m_panels) {
            m_panels->clipboard_changed();
        }

        m_thumbnails.follow_clipboard(m_history, m_damage);
    }

    m_history_changed = false;
}

/* A chosen entry becomes current, and pasting sends the focused window the paste event after its types */
void server::choose_clipboard_entry(uint64_t id, bool pastes) {
    m_history.make_current(id, monotonic_ns());
    note_clipboard_change();
    if (pastes) {
        paste_into_focus();
    }
}

/* A watcher behind the clipboard's generation hears the newest types only from an empty out queue, as answers leave,
 * so a client copying in a loop never grows the queue of a watcher that stopped reading */
void server::catch_up_watcher(dm_client& c) {
    if (c.watches_clipboard && c.clipboard_generation != m_clipboard_generation && c.out_q.empty() && !c.dead) {
        send_clipboard_types(c);
    }
}

/* The copier of the current item hears that it is its own, so it can answer its own pastes from the copy it made */
void server::send_clipboard_types(dm_client& c) {
    swp_clipboard_types m = describe_types(m_history.current());
    uint16_t flags = &c == m_clipboard_owner ? SWP_CLIPBOARD_TYPES_OWN : 0;
    send_to(c, SWP_MSG_CLIPBOARD_TYPES, &m, sizeof(m), flags);
    c.clipboard_generation = m_clipboard_generation;
}

/* A client's copies carry its launcher's label, as the bar names it, else its app id */
std::string server::client_label(const dm_client& c) const {
    for (uint32_t i = 0; i < m_conf->pin_count; i++) {
        const stlxconf_pin_t& pin = m_conf->pins[i];
        if (launches_app(pin, c.app_id) && pin.label[0]) {
            return pin.label;
        }
    }

    return c.app_id;
}

/* Takes an offer's objects and checks them as they arrive, ending a client whose objects do not hold up, and copies a
 * small clipboard item into objects of stlxdm's own */
bool server::take_offer(dm_client& c, std::vector<clipboard_rep>& reps, bool owns_small) {
    intake_error error = take_offer_objects(reps, handles_of(c));
    if (error != intake_error::none) {
        printf("stlxdm: ending %s, whose offered objects %s\r\n", c.app_id, intake_error_text(error));
        c.dead = true;
        return false;
    }

    if (owns_small) {
        own_small_item(reps, make_shared_object);
    }

    return true;
}

/* A copy with text becomes an item of one text representation */
void server::set_clipboard(dm_client& c, const uint8_t* payload) {
    swp_clipboard_set m;
    memcpy(&m, payload, sizeof(m));

    std::vector<clipboard_rep> reps;
    if (m.len > 0) {
        reps.push_back({ SWP_MIME_TEXT, m.len, -1 });
    }

    if (take_offer(c, reps, true)) {
        replace_clipboard(clipboard_item(std::move(reps), 0), client_label(c));
        m_clipboard_owner = &c;
    }
}

/* A malformed offer ends the client, and flags it does not know are dropped */
void server::offer_clipboard(dm_client& c, const uint8_t* payload) {
    swp_offer m;
    memcpy(&m, payload, sizeof(m));

    std::vector<clipboard_rep> reps;
    if (!read_offer(m, &reps)) {
        c.dead = true;
        return;
    }

    if (take_offer(c, reps, true)) {
        replace_clipboard(clipboard_item(std::move(reps), m.flags & SWP_CLIPBOARD_PRIVATE), client_label(c));
        m_clipboard_owner = &c;
    }
}

/* An ask without a type asks for text */
void server::ask_clipboard(dm_client& c, const swp_header& hdr, const uint8_t* payload) {
    if (c.clipboard_asks.size() == CLIPBOARD_ASKS_MAX) {
        c.dead = true;
        return;
    }

    std::string mime_type = SWP_MIME_TEXT;
    if (hdr.length == sizeof(swp_clipboard_get)) {
        swp_clipboard_get m;
        memcpy(&m, payload, sizeof(m));
        mime_type.assign(m.mime, strnlen(m.mime, sizeof(m.mime)));
    }

    c.clipboard_asks.push_back(std::move(mime_type));
    send_clipboard_answers(c);
}

/* Makes `text` the clipboard, as a client's CLIPBOARD_SET does, keeping the old clipboard when memory runs out */
void server::copy_text(const std::string& text) {
    int object = make_shared_object(text.data(), text.size());
    if (object < 0) {
        printf("stlxdm: copying %zu bytes failed, errno %d\r\n", text.size(), errno);
        return;
    }

    replace_clipboard(clipboard_item({ { SWP_MIME_TEXT, text.size(), object } }, 0), QUICK_ACTIONS_LABEL);
}

/* Copies a file as File Manager's copies are, so it pastes as a file where files paste and as its path elsewhere */
void server::copy_file(const std::string& path) {
    std::string list = file_uri_list(path);
    int list_object = make_shared_object(list.data(), list.size());
    int text_object = list_object >= 0 ? make_shared_object(path.data(), path.size()) : -1;
    if (text_object < 0) {
        printf("stlxdm: copying %s failed, errno %d\r\n", path.c_str(), errno);
        if (list_object >= 0) {
            close(list_object);
        }

        return;
    }

    replace_clipboard(clipboard_item({ { "text/uri-list", list.size(), list_object },
                                       { SWP_MIME_TEXT, path.size(), text_object } }, 0), QUICK_ACTIONS_LABEL);
}

/* Sends a message whose one handle, unless `handle` is -1, rides its first byte, so it leaves only from an empty out_q,
 * queueing what the socket leaves. Returns false when the socket takes none of it. */
bool server::send_with_handle(dm_client& c, uint16_t type, const void* payload, uint32_t length, int handle) {
    swp_header hdr = { type, 0, length };
    uint8_t bytes[SWP_MAX_MSG_SIZE];
    size_t total = sizeof(hdr) + length;
    memcpy(bytes, &hdr, sizeof(hdr));
    memcpy(bytes + sizeof(hdr), payload, length);

    iovec iov = { bytes, total };
    msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    alignas(cmsghdr) uint8_t control[CMSG_SPACE(sizeof(int))] = {};
    if (handle >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        cmsghdr* head = CMSG_FIRSTHDR(&msg);
        head->cmsg_level = SOL_SOCKET;
        head->cmsg_type = SCM_RIGHTS;
        head->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(head), &handle, sizeof(int));
    }

    ssize_t n = sendmsg(c.fd, &msg, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            c.dead = true;
        }

        return false;
    }

    c.out_q.insert(c.out_q.end(), bytes + n, bytes + total);

    return true;
}

/* A handle to `object` that its receiver can only read, so no reader can change what other readers get, or -1 when
 * `object` is -1 or no handle can be made. The caller closes it once sent. */
int server::read_only_copy(int object) {
    return object >= 0 ? stlx_handle_restrict(object, STLX_RIGHT_READ) : -1;
}

/* Sends the oldest ask's CLIPBOARD_DATA from an empty out_q, and nothing for an object its source has shrunk since */
bool server::send_clipboard_answer(dm_client& c) {
    const clipboard_rep* found = m_history.current().find(c.clipboard_asks.front());
    int object = found && holds_length(*found) ? read_only_copy(found->object) : -1;
    swp_clipboard_data m = { object >= 0 ? found->len : 0 };

    bool is_sent = send_with_handle(c, SWP_MSG_CLIPBOARD_DATA, &m, sizeof(m), object);
    if (object >= 0) {
        close(object);
    }

    return is_sent;
}

/* Sends a message to `c` behind the events `w` staged, so the client reads them in the order they happened */
void server::send_window_message(dm_client& c, dm_window* w, uint16_t type, const void* payload, uint32_t length) {
    if (w) {
        flush_window_events(c, *w);
    }

    send_to(c, type, payload, length);
}

void server::send_clipboard_answers(dm_client& c) {
    while (!c.clipboard_asks.empty() && c.out_q.empty() && !c.dead) {
        if (!send_clipboard_answer(c)) {
            return;
        }

        c.clipboard_asks.pop_front();
    }
}

