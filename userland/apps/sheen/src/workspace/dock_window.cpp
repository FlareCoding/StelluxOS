#include "workspace/dock_window.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>

namespace sheen {

static constexpr int FLOAT_DEFAULT_W = 760;
static constexpr int FLOAT_DEFAULT_H = 420;
static constexpr int FLOAT_MIN_W = 420;
static constexpr int FLOAT_MIN_H = 220;
static constexpr int ACTION_BAR_HEIGHT = 34;
static constexpr int ACTION_BAR_PADDING_X = 10;

namespace {

class dock_window_toolbar : public QWidget {
public:
    dock_window_toolbar(ui::ui_pill_button* dock_back, QWidget* parent) : QWidget(parent) {
        setFixedHeight(ACTION_BAR_HEIGHT);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(ACTION_BAR_PADDING_X, 0, ACTION_BAR_PADDING_X, 1);
        layout->addStretch(1);
        layout->addWidget(dock_back, 0, Qt::AlignVCenter);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), token(STLXTHEME_SHADE));
        painter.fillRect(QRect(0, height() - 1, width(), 1), token(STLXTHEME_LINE));
    }
};

} // namespace

dock_window::dock_window(const QString& panel_key, const QString& title) : m_panel_key(panel_key) {
    setWindowTitle(title);
    resize(FLOAT_DEFAULT_W, FLOAT_DEFAULT_H);
    setMinimumSize(FLOAT_MIN_W, FLOAT_MIN_H);

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* dock_back = new ui::ui_pill_button(QStringLiteral("Dock Back"), this);
    connect(dock_back, &QAbstractButton::clicked, this, [this] {
        if (on_dock_back_requested) {
            std::function<void(bool)> callback = on_dock_back_requested;
            on_dock_back_requested = nullptr;
            callback(false);
        }
    });

    root_layout->addWidget(new dock_window_toolbar(dock_back, this));

    auto* body = new QWidget(this);
    m_body_layout = new QVBoxLayout(body);
    m_body_layout->setContentsMargins(0, 0, 0, 0);
    m_body_layout->setSpacing(0);
    root_layout->addWidget(body, 1);
}

dock_window::~dock_window() = default;

void dock_window::take_panel_widget(QWidget* panel) {
    m_panel_widget = panel;
    if (!panel) {
        return;
    }

    m_body_layout->addWidget(panel, 1);
    panel->show();
}

QWidget* dock_window::release_panel_widget() {
    QWidget* panel = m_panel_widget;
    m_panel_widget = nullptr;
    if (panel) {
        m_body_layout->removeWidget(panel);
    }

    return panel;
}

void dock_window::closeEvent(QCloseEvent* event) {
    if (QWidget* popup = QApplication::activePopupWidget()) {
        popup->close();
    }

    if (!m_closing_from_host && on_dock_back_requested) {
        std::function<void(bool)> callback = on_dock_back_requested;
        on_dock_back_requested = nullptr;
        callback(true);
    }

    event->accept();
}

void dock_window::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_OPAQUE_POCKET));
}

} // namespace sheen
