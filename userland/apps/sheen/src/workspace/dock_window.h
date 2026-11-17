#ifndef SHEEN_WORKSPACE_DOCK_WINDOW_H
#define SHEEN_WORKSPACE_DOCK_WINDOW_H

#include <QString>
#include <QWidget>

#include <functional>

class QVBoxLayout;

namespace sheen {

/* Top-level Razor window temporarily hosting an alien dock panel widget during Pop-Out. */
class dock_window : public QWidget {
public:
    dock_window(const QString& panel_key, const QString& title);
    ~dock_window() override;

    const QString& panel_key() const { return m_panel_key; }
    void take_panel_widget(QWidget* panel);
    QWidget* release_panel_widget();
    void set_closing_from_host(bool closing) { m_closing_from_host = closing; }

    std::function<void(bool from_window_close)> on_dock_back_requested;

protected:
    void closeEvent(QCloseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_panel_key;
    QVBoxLayout* m_body_layout = nullptr;
    QWidget* m_panel_widget = nullptr;
    bool m_closing_from_host = false;
};

} // namespace sheen

#endif
