#ifndef SHEEN_WORKSPACE_SHEEN_WINDOW_H
#define SHEEN_WORKSPACE_SHEEN_WINDOW_H

#include "app/settings.h"

#include <QMainWindow>
#include <QString>
#include <QStringList>

namespace sheen {

class editor_core;
class sheen_app;

/* Top-level Sheen window hosting an editor_core surface and its window-wide status bar. */
class sheen_window : public QMainWindow {
public:
    explicit sheen_window(sheen_app& app);
    ~sheen_window() override;

    editor_core& editor() { return *m_editor; }
    const editor_core& editor() const { return *m_editor; }

    void open_paths(const QStringList& paths);
    void receive_paths(const QStringList& paths);

    bool confirm_close(bool is_quitting_app = false);
    void reset_close_confirmation() { m_close_confirmed = false; }
    void apply_settings(const sheen_settings& settings, const QStringList& problems);
    void refresh_empty_state();
    void on_dependencies_changed();

    QString window_label() const;
    QString window_note() const;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    sheen_app& m_app;
    editor_core* m_editor = nullptr;
    bool m_close_confirmed = false;
};

} // namespace sheen

#endif
