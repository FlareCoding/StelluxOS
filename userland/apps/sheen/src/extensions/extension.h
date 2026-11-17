#ifndef SHEEN_EXTENSIONS_EXTENSION_H
#define SHEEN_EXTENSIONS_EXTENSION_H

#include "workspace/event_stream.h"

#include <QByteArrayView>
#include <QSet>
#include <QString>

#include <memory>
#include <span>

class QWidget;

namespace sheen {

class editor_core;
class extension_host;
class extension_session;

enum class extension_icon { CHIP, CODE, PYTHON, BINARY };

/* Shallow snapshot of a workspace folder's top-level entries, captured with one non-recursive readdir. */
struct workspace_files {
    QString root_path;
    QString folder_name;
    QSet<QString> entry_names;

    bool contains(const QString& name) const { return entry_names.contains(name); }
    bool has_suffix(const QString& suffix) const;
};

/* One stlxpkg package paired with a file it places, so image-baked packages are never reinstalled. */
struct package_requirement {
    const char* package_name;
    const char* probe_path; // checked with access(F_OK), skips stlxpkg install when already on disk
};

/* One group on the Extension Setup tab, satisfied by stlxpkg packages, a post-install command, or both. */
struct dependency_group {
    const char* id;
    const char* title;
    const char* summary;
    std::span<const package_requirement> packages;
    std::span<const char* const> post_install_argv;
    const char* post_install_probe_path;
    event_protocol post_install_protocol = event_protocol::NONE;
    bool is_required = true;
};

/* Direct viewer for binary or trace files in Plain or Workspace windows. */
struct file_opener_spec {
    const char* id;
    const char* action_label;
    bool (*can_open_file)(const QString& path, QByteArrayView header_bytes) = nullptr;
    QWidget* (*create_file_view)(editor_core& editor, const QString& path) = nullptr;
};

struct extension_descriptor {
    const char* id;
    const char* name;
    const char* summary;
    extension_icon icon;
    const char* detection_notice;
    bool auto_offer = false;
    std::span<const dependency_group> dependencies;
    std::span<const file_opener_spec> file_openers;

    bool (*qualifies_workspace)(const workspace_files& files) = nullptr;
    std::unique_ptr<extension_session> (*create_session)(extension_host& host) = nullptr;
};

bool is_dependency_group_installed(const dependency_group& group);
bool are_dependencies_installed(const extension_descriptor& descriptor);

} // namespace sheen

#endif
