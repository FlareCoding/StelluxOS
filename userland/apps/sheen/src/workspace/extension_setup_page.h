#ifndef SHEEN_WORKSPACE_EXTENSION_SETUP_PAGE_H
#define SHEEN_WORKSPACE_EXTENSION_SETUP_PAGE_H

#include "extensions/extension.h"

#include <QString>
#include <QWidget>

#include <functional>

class QVBoxLayout;

namespace sheen {

class sheen_app;

namespace ui {
class ui_log_panel;
}

/* Custom editor tab displaying extension dependency groups, live installation progress, and retry controls. */
class extension_setup_page : public QWidget {
public:
    extension_setup_page(sheen_app& app, const QString& focus_extension_id, QWidget* parent = nullptr);
    ~extension_setup_page() override;

    void focus_extension(const QString& extension_id);
    void refresh_state();

    std::function<void(const QString& extension_id)> on_extension_ready;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void start_install(const extension_descriptor& descriptor);

    sheen_app& m_app;
    QString m_focus_extension_id;
    QString m_pending_enable_id;
    QWidget* m_cards_container = nullptr;
    QVBoxLayout* m_cards_layout = nullptr;
    ui::ui_log_panel* m_log = nullptr;
};

} // namespace sheen

#endif
