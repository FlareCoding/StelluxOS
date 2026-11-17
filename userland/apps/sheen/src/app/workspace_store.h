#ifndef SHEEN_APP_WORKSPACE_STORE_H
#define SHEEN_APP_WORKSPACE_STORE_H

#include <string>
#include <string_view>
#include <vector>

namespace sheen {

inline constexpr int MIN_DOCK_HEIGHT = 80;
inline constexpr int MAX_DOCK_HEIGHT = 800;
inline constexpr int DEFAULT_DOCK_HEIGHT = 204;
inline constexpr int MIN_SIDEBAR_WIDTH = 140;
inline constexpr int MAX_SIDEBAR_WIDTH = 600;
inline constexpr int DEFAULT_SIDEBAR_WIDTH = 216;

struct workspace_record {
    std::string root_path;
    std::vector<std::string> enabled_extensions;
    std::vector<std::string> dismissed_offers;
    std::string pinned_strip_extension;
    int dock_height = DEFAULT_DOCK_HEIGHT;
    bool is_dock_collapsed = false;
    std::string active_dock_panel;
    bool is_sidebar_visible = true;
    int sidebar_width = DEFAULT_SIDEBAR_WIDTH;
};

/* Persists recent paths and per-workspace UI and extension state under /var/lib/sheen without touching user folders. */
class workspace_store {
public:
    explicit workspace_store(std::string base_dir = "/var/lib/sheen");

    const std::string& base_dir() const { return m_base_dir; }

    std::vector<std::string> load_recent_paths() const;
    bool record_recent_path(std::string_view path);
    bool clear_recent_paths();

    workspace_record load_workspace(std::string_view root_path) const;
    bool save_workspace(const workspace_record& record);

    static std::string normalize_workspace_path(std::string_view root_path);
    static std::string workspace_file_key(std::string_view root_path);

private:
    std::string recent_file_path() const;
    std::string workspace_conf_path(std::string_view normalized_root) const;

    std::string m_base_dir;
};

} // namespace sheen

#endif
