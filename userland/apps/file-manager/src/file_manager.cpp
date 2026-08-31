/* file-manager: a directory browser on the toolkit. A toolbar holds
 * the up control and an editable path, rows carry a type glyph with
 * name and size, and entries open on double click, spawn when
 * executable, or fall to the status line. Structural changes defer
 * to a zero timer so no widget rebuilds the tree from its own
 * handler.
 */
#include <stlxui/stlxui.h>
#include <stlxwin/stlxwin.h>

#include <stlx/proc.h>
#include <stlxgfx/bmp.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

constexpr int32_t ROW_H = 26;
constexpr int32_t GLYPH_PX = 16;
constexpr int32_t ROW_RADIUS = 4;
constexpr uint16_t USAGE_ENTER = 0x28;
constexpr uint16_t USAGE_DELETE = 0x4C;
constexpr uint16_t USAGE_UP = 0x52;
constexpr uint16_t USAGE_DOWN = 0x51;

/* Entry kinds map one to one onto the row glyphs */
enum class fm_kind : uint8_t { folder, file, text, exec, image };

static const char* const KIND_ICON[] = {
    "/etc/res/icons/icon_fm_folder_16x16.bmp",
    "/etc/res/icons/icon_fm_file_16x16.bmp",
    "/etc/res/icons/icon_fm_text_16x16.bmp",
    "/etc/res/icons/icon_fm_exec_16x16.bmp",
    "/etc/res/icons/icon_fm_image_16x16.bmp",
};

struct fm_entry {
    std::string name;
    uint64_t size = 0;
    fm_kind kind = fm_kind::file;
};

namespace {

class fm_row;

}

/* Deferred structural actions, run from a zero timer after dispatch */
enum class fm_action : uint8_t { none, open, remove };

struct app_state {
    ui::app* app = nullptr;
    ui::window_host* win = nullptr;
    std::string cwd = "/";
    std::vector<fm_entry> entries;
    std::vector<fm_row*> rows;
    int32_t selected = -1;
    ui::text_input* path_field = nullptr;
    ui::box* list = nullptr;
    ui::scroll_view* scroll = nullptr;
    ui::label* status = nullptr;
    stlxgfx_surface_t* glyphs[5] = {};
    fm_action pending = fm_action::none;
    int32_t pending_index = -1;
    std::string pending_path;
};

static app_state g_st;

static void set_status(const std::string& text) {
    if (g_st.status) {
        g_st.status->set_text(text);
    }
}

static void select_row(int32_t index);
static void request_open(int32_t index);
static void request_remove(int32_t index);
static void open_row_menu(ui::widget* anchor, int32_t index);

namespace {

/* One directory entry: glyph, name, and a right aligned size. Click
 * selects, double click opens, the menu and the delete key act on
 * the selection. */
class fm_row : public ui::widget {
public:
    fm_row(int32_t index, const fm_entry& entry)
        : m_index(index), m_entry(&entry) {}

    void set_selected(bool v) {
        if (m_selected == v) {
            return;
        }

        m_selected = v;
        invalidate();
    }

    bool focusable() const override { return true; }

    ui::size measure(ui::size) override {
        return { 0, ROW_H };
    }

    void paint(ui::painter& p) override {
        const ui::theme& t = ui::theme::active();

        if (m_selected) {
            p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, ROW_RADIUS,
                           t.surface);
            p.rounded_rect({ 0, 5, 3, m_frame.h - 10 }, 2, t.accent);
        } else if (m_hover) {
            p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, ROW_RADIUS,
                           0xFF232334);
        }

        stlxgfx_surface_t* glyph =
            g_st.glyphs[static_cast<size_t>(m_entry->kind)];
        if (glyph) {
            p.image({ 10, (m_frame.h - GLYPH_PX) / 2 }, glyph);
        }

        ui::size name = p.measure_text(m_entry->name, 0);
        int32_t baseline = (m_frame.h - name.h) / 2 + p.font_ascent(0);
        p.text({ 36, baseline }, m_entry->name, 0, t.text);

        if (m_entry->kind != fm_kind::folder) {
            char size_text[32];
            format_size(size_text, sizeof(size_text), m_entry->size);
            ui::size sz = p.measure_text(size_text, 0);
            p.text({ m_frame.w - sz.w - 12, baseline }, size_text, 0,
                   t.text_dim);
        }
    }

    void on_pointer_enter() override {
        m_hover = true;
        invalidate();
    }

    void on_pointer_leave() override {
        m_hover = false;
        invalidate();
    }

    bool on_pointer_down(const ui::pointer_event& e) override {
        select_row(m_index);
        focus();

        if (e.button == 1) {
            open_row_menu(this, m_index);
            return true;
        }

        if (e.button == 0 && e.clicks >= 2) {
            request_open(m_index);
        }

        return true;
    }

    bool on_key_down(const ui::key_event& e) override {
        switch (e.usage) {
        case USAGE_ENTER:
            request_open(m_index);
            return true;
        case USAGE_DELETE:
            request_remove(m_index);
            return true;
        case USAGE_UP:
            select_row(m_index - 1);
            return true;
        case USAGE_DOWN:
            select_row(m_index + 1);
            return true;
        default:
            return false;
        }
    }

private:
    static void format_size(char* out, size_t cap, uint64_t bytes) {
        if (bytes >= 1024ull * 1024ull) {
            snprintf(out, cap, "%llu.%llu M",
                     (unsigned long long)(bytes / (1024 * 1024)),
                     (unsigned long long)(bytes % (1024 * 1024) * 10
                                          / (1024 * 1024)));
        } else if (bytes >= 1024) {
            snprintf(out, cap, "%llu.%llu K",
                     (unsigned long long)(bytes / 1024),
                     (unsigned long long)(bytes % 1024 * 10 / 1024));
        } else {
            snprintf(out, cap, "%llu B", (unsigned long long)bytes);
        }
    }

    int32_t m_index;
    const fm_entry* m_entry;
    bool m_selected = false;
    bool m_hover = false;
};

} // namespace

/* The extension decides the glyph for regular files, the mode bits
 * decide executables */
static fm_kind classify(const std::string& name, const struct stat& st) {
    if (S_ISDIR(st.st_mode)) {
        return fm_kind::folder;
    }

    if (st.st_mode & 0111) {
        return fm_kind::exec;
    }

    size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = name.substr(dot + 1);
        for (char& c : ext) {
            c = static_cast<char>(tolower(c));
        }

        if (ext == "bmp" || ext == "png" || ext == "jpg" ||
            ext == "jpeg") {
            return fm_kind::image;
        }
        if (ext == "txt" || ext == "conf" || ext == "md" ||
            ext == "log" || ext == "cfg" || ext == "ini") {
            return fm_kind::text;
        }
    }

    return fm_kind::file;
}

static std::string join_path(const std::string& dir,
                             const std::string& name) {
    if (dir == "/") {
        return "/" + name;
    }

    return dir + "/" + name;
}

static std::string parent_path(const std::string& dir) {
    size_t slash = dir.rfind('/');
    if (slash == std::string::npos || slash == 0) {
        return "/";
    }

    return dir.substr(0, slash);
}

/* Reads one directory into the entry list, directories first, both
 * groups alphabetical */
static int load_dir(const std::string& path,
                    std::vector<fm_entry>& out) {
    DIR* d = opendir(path.c_str());
    if (!d) {
        return -1;
    }

    out.clear();
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        struct stat st;
        std::string full = join_path(path, de->d_name);
        if (stat(full.c_str(), &st) != 0) {
            continue;
        }

        fm_entry e;
        e.name = de->d_name;
        e.size = static_cast<uint64_t>(st.st_size);
        e.kind = classify(e.name, st);
        out.push_back(std::move(e));
    }
    closedir(d);

    std::sort(out.begin(), out.end(),
              [](const fm_entry& a, const fm_entry& b) {
                  bool ad = a.kind == fm_kind::folder;
                  bool bd = b.kind == fm_kind::folder;
                  if (ad != bd) {
                      return ad;
                  }
                  return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
              });

    return 0;
}

static void rebuild_list() {
    while (!g_st.rows.empty()) {
        g_st.list->remove(g_st.rows.back());
        g_st.rows.pop_back();
    }
    g_st.selected = -1;

    for (size_t i = 0; i < g_st.entries.size(); i++) {
        fm_row* row = g_st.list->add<fm_row>(static_cast<int32_t>(i),
                                             g_st.entries[i]);
        row->s().main = ui::length::fixed(ROW_H);
        g_st.rows.push_back(row);
    }

    g_st.path_field->set_text(g_st.cwd);

    char text[64];
    snprintf(text, sizeof(text), "%zu items", g_st.entries.size());
    set_status(text);
}

static void navigate(const std::string& path) {
    std::vector<fm_entry> entries;
    if (load_dir(path, entries) != 0) {
        set_status("cannot open " + path);
        return;
    }

    g_st.cwd = path;
    g_st.entries = std::move(entries);
    rebuild_list();
}

static void select_row(int32_t index) {
    if (index < 0 || index >= static_cast<int32_t>(g_st.rows.size())) {
        return;
    }

    if (g_st.selected >= 0 &&
        g_st.selected < static_cast<int32_t>(g_st.rows.size())) {
        g_st.rows[g_st.selected]->set_selected(false);
    }
    g_st.selected = index;
    g_st.rows[index]->set_selected(true);
    g_st.rows[index]->focus();

    const fm_entry& e = g_st.entries[index];
    set_status(e.name);
}

/* Deferred openers: the timer callback runs after the dispatching
 * widget has fully unwound, so rebuilding the list is safe */
static void run_pending() {
    fm_action action = g_st.pending;
    int32_t index = g_st.pending_index;
    g_st.pending = fm_action::none;
    g_st.pending_index = -1;

    if (index < 0 || index >= static_cast<int32_t>(g_st.entries.size())) {
        return;
    }

    const fm_entry& e = g_st.entries[index];
    std::string full = join_path(g_st.cwd, e.name);

    if (action == fm_action::open) {
        if (e.kind == fm_kind::folder) {
            navigate(full);
            return;
        }

        if (e.kind == fm_kind::exec) {
            int handle = proc_exec(full.c_str(), nullptr);
            if (handle >= 0) {
                proc_detach(handle);
                set_status("launched " + e.name);
            } else {
                set_status("failed to launch " + e.name);
            }
            return;
        }

        set_status("no handler for " + e.name);
        return;
    }

    if (action == fm_action::remove) {
        int rc = e.kind == fm_kind::folder ? rmdir(full.c_str())
                                           : unlink(full.c_str());
        if (rc != 0) {
            set_status(e.kind == fm_kind::folder
                       ? "cannot remove " + e.name +
                         " (not empty or protected)"
                       : "cannot remove " + e.name);
            return;
        }

        set_status("removed " + e.name);
        navigate(g_st.cwd);
    }
}

static void schedule(fm_action action, int32_t index) {
    g_st.pending = action;
    g_st.pending_index = index;
    g_st.app->set_timer(1, run_pending);
}

static void request_open(int32_t index) {
    schedule(fm_action::open, index);
}

static void request_remove(int32_t index) {
    schedule(fm_action::remove, index);
}

static void open_row_menu(ui::widget* anchor, int32_t index) {
    std::vector<ui::menu_item> items;
    items.push_back({ "Open", [index]() { request_open(index); },
                      true, true });
    items.push_back({ "Delete permanently",
                      [index]() { request_remove(index); },
                      true, false });

    ui::menu::open_at(anchor, std::move(items));
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    ui::app app("file-manager");
    if (!app.ok()) {
        printf("file-manager: no display manager\r\n");
        return 1;
    }
    g_st.app = &app;

    g_st.win = app.create_window(560, 420, "File Manager",
                                 STLXWIN_WF_RESIZABLE);
    if (!g_st.win) {
        printf("file-manager: window creation failed\r\n");
        return 1;
    }
    stlxwin_window_set_min_size(g_st.win->window(), 420, 300);

    for (size_t i = 0; i < 5; i++) {
        g_st.glyphs[i] = stlxgfx_load_bmp(KIND_ICON[i]);
    }

    const ui::theme& t = ui::theme::active();
    auto root = std::make_unique<ui::box>(ui::axis::column);
    root->s().background = t.window_bg;

    /* The toolbar: up one level, then the editable location */
    ui::box* bar = root->add<ui::box>(ui::axis::row);
    bar->s().main = ui::length::content();
    bar->s().padding = ui::edge_insets::all(10);
    bar->s().gap = 8;
    bar->s().align_items = ui::align::center;

    ui::button* up = bar->add<ui::button>("Up");
    up->s().main = ui::length::content();
    up->on_click = []() {
        navigate(parent_path(g_st.cwd));
    };

    g_st.path_field = bar->add<ui::text_input>();
    g_st.path_field->s().main = ui::length::flex();
    g_st.path_field->on_submit = [](const std::string& path) {
        navigate(path);
    };

    g_st.scroll = root->add<ui::scroll_view>();
    g_st.scroll->s().main = ui::length::flex();

    g_st.list = g_st.scroll->add<ui::box>(ui::axis::column);
    g_st.list->s().main = ui::length::content();
    g_st.list->s().padding = ui::edge_insets::xy(10, 4);
    g_st.list->s().gap = 2;

    ui::box* footer = root->add<ui::box>(ui::axis::row);
    footer->s().main = ui::length::content();
    footer->s().padding = ui::edge_insets::xy(12, 6);
    footer->s().background = 0xFF181825;

    g_st.status = footer->add<ui::label>("");
    g_st.status->set_font_size(12);
    g_st.status->set_color(t.text_dim);
    g_st.status->s().main = ui::length::content();

    g_st.win->set_root(std::move(root));
    navigate("/");

    g_st.win->on_close = [&app]() {
        app.quit(0);
        return true;
    };

    return app.run();
}
