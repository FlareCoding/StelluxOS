#include "workspace/extensions_popover.h"
#include "extensions/registry.h"
#include "ui/chrome.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>

namespace sheen {

static constexpr int POPOVER_WIDTH = 384;
static constexpr int MAX_POPOVER_HEIGHT = 420;
static constexpr int HEADER_HEIGHT = 48;
static constexpr int FOOTER_HEIGHT = 40;
static constexpr int EXTENSION_ROW_HEIGHT = 68;
static constexpr int EMPTY_BODY_HEIGHT = 56;
static constexpr int INSET_X = 14;
static constexpr int ICON_BOX_SIZE = 24;
static constexpr int DETECTED_BADGE_HEIGHT = 18;
static constexpr int DETECTED_BADGE_PADDING_X = 12;

namespace {

class icon_glyph_widget : public QWidget {
public:
    explicit icon_glyph_widget(extension_icon icon, QWidget* parent) : QWidget(parent), m_icon(icon) {
        setFixedSize(ICON_BOX_SIZE, ICON_BOX_SIZE);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(token(STLXTHEME_TEXT_SECONDARY), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);

        switch (m_icon) {
            case extension_icon::CHIP:
                painter.drawRoundedRect(QRectF(6, 6, 12, 12), 2, 2);
                for (int offset : { 9, 15 }) {
                    painter.drawLine(QPointF(offset, 3), QPointF(offset, 6));
                    painter.drawLine(QPointF(offset, 18), QPointF(offset, 21));
                    painter.drawLine(QPointF(3, offset), QPointF(6, offset));
                    painter.drawLine(QPointF(18, offset), QPointF(21, offset));
                }

                break;
            case extension_icon::CODE:
                painter.drawPolyline(QPolygonF({ QPointF(9, 7), QPointF(4, 12), QPointF(9, 17) }));
                painter.drawPolyline(QPolygonF({ QPointF(15, 7), QPointF(20, 12), QPointF(15, 17) }));
                break;
            case extension_icon::PYTHON:
                painter.drawRoundedRect(QRectF(5, 5, 14, 14), 4, 4);
                painter.drawEllipse(QPointF(9.5, 9.5), 1.2, 1.2);
                painter.drawEllipse(QPointF(14.5, 14.5), 1.2, 1.2);
                break;
            case extension_icon::BINARY:
                painter.drawRoundedRect(QRectF(5, 4, 14, 16), 2, 2);
                painter.drawLine(QPointF(8, 9), QPointF(16, 9));
                painter.drawLine(QPointF(8, 13), QPointF(14, 13));
                break;
        }
    }

private:
    extension_icon m_icon;
};

class detected_badge : public QWidget {
public:
    explicit detected_badge(QWidget* parent) : QWidget(parent) {
        QFont font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::Medium);
        setFont(font);
        int width = fontMetrics().horizontalAdvance(QStringLiteral("Detected")) + DETECTED_BADGE_PADDING_X;
        setFixedSize(width, DETECTED_BADGE_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor tint = token(STLXTHEME_ACCENT);
        tint.setAlphaF(0.16);

        painter.setPen(QPen(token(STLXTHEME_ACCENT), 1));
        painter.setBrush(tint);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                STLXTHEME_CHIP_RADIUS, STLXTHEME_CHIP_RADIUS);
        painter.setPen(token(STLXTHEME_ACCENT));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Detected"));
    }
};

class hairline_widget : public QWidget {
public:
    explicit hairline_widget(QWidget* parent) : QWidget(parent) {
        setFixedHeight(1);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), token(STLXTHEME_LINE));
    }
};

} // namespace

int extensions_popover::compute_popover_height() {
    std::span<const extension_descriptor> exts = registered_extensions();
    int body = exts.empty() ? EMPTY_BODY_HEIGHT : static_cast<int>(exts.size()) * EXTENSION_ROW_HEIGHT;
    return qMin(MAX_POPOVER_HEIGHT, HEADER_HEIGHT + body + FOOTER_HEIGHT + 12);
}

extensions_popover::extensions_popover(QWidget* anchor, const workspace_files& files, const QSet<QString>& enabled_ids)
    : ui::ui_popover(anchor, POPOVER_WIDTH, compute_popover_height()), m_files(files), m_enabled_ids(enabled_ids) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(1, 8, 1, 8);
    root_layout->setSpacing(4);

    auto* header = new QWidget(this);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(INSET_X, 0, INSET_X, 4);
    header_layout->setSpacing(1);

    auto* title = new QLabel(QStringLiteral("Extensions"), header);
    title->setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::DemiBold));
    title->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(token(STLXTHEME_TEXT).name()));
    header_layout->addWidget(title);

    auto* path = new QLabel(files.root_path, header);
    path->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    path->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(token(STLXTHEME_TEXT_DIM).name()));
    header_layout->addWidget(path);
    root_layout->addWidget(header);
    root_layout->addWidget(new hairline_widget(this));

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    auto* rows_host = new QWidget(scroll);
    rows_host->setStyleSheet(QStringLiteral("background: transparent;"));
    m_rows_layout = new QVBoxLayout(rows_host);
    m_rows_layout->setContentsMargins(INSET_X, 4, INSET_X, 4);
    m_rows_layout->setSpacing(6);

    scroll->setWidget(rows_host);
    root_layout->addWidget(scroll, 1);

    root_layout->addWidget(new hairline_widget(this));

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(INSET_X, 2, INSET_X, 0);
    footer->addStretch(1);

    auto* setup_button = new ui::ui_pill_button(QStringLiteral("Extension Setup..."), this);
    connect(setup_button, &QAbstractButton::clicked, this, [this] {
        std::function<void(const QString&)> callback = on_setup_requested;
        close();
        if (callback) {
            callback(QString());
        }
    });

    footer->addWidget(setup_button);
    root_layout->addLayout(footer);

    refresh_rows(enabled_ids);
}

void extensions_popover::refresh_rows(const QSet<QString>& enabled_ids) {
    m_enabled_ids = enabled_ids;
    while (QLayoutItem* item = m_rows_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }

        delete item;
    }

    std::span<const extension_descriptor> extensions = registered_extensions();
    if (extensions.empty()) {
        auto* empty = new QLabel(QStringLiteral("No workspace extensions registered."), this);
        empty->setFont(ui_font(STLXTHEME_BODY_SIZE));
        empty->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg(token(STLXTHEME_TEXT_DIM).name()));
        m_rows_layout->addWidget(empty);
        m_rows_layout->addStretch(1);
        return;
    }

    for (const extension_descriptor& desc : extensions) {
        QString ext_id = QString::fromLatin1(desc.id);
        bool detected = desc.qualifies_workspace && desc.qualifies_workspace(m_files);
        bool installed = are_dependencies_installed(desc);

        auto* row = new QWidget(this);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 2, 0, 2);
        row_layout->setSpacing(10);
        row_layout->addWidget(new icon_glyph_widget(desc.icon, row), 0, Qt::AlignTop);

        auto* text_col = new QVBoxLayout;
        text_col->setContentsMargins(0, 0, 0, 0);
        text_col->setSpacing(2);

        auto* name_row = new QHBoxLayout;
        name_row->setSpacing(6);
        auto* name = new QLabel(QString::fromLatin1(desc.name), row);
        name->setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::DemiBold));
        name->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(token(STLXTHEME_TEXT).name()));

        name_row->addWidget(name);
        if (detected) {
            name_row->addWidget(new detected_badge(row));
        }

        name_row->addStretch(1);
        text_col->addLayout(name_row);

        auto* summary = new QLabel(QString::fromLatin1(desc.summary), row);
        summary->setFont(ui_font(STLXTHEME_CAPTION_SIZE));
        summary->setWordWrap(true);
        summary->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                   .arg(token(STLXTHEME_TEXT_SECONDARY).name()));
        text_col->addWidget(summary);
        row_layout->addLayout(text_col, 1);

        if (installed) {
            auto* toggle = new ui::ui_switch(row);
            toggle->setChecked(m_enabled_ids.contains(ext_id));
            connect(toggle, &QAbstractButton::toggled, row, [this, ext_id](bool checked) {
                if (on_toggled) {
                    on_toggled(ext_id, checked);
                }
            });
            row_layout->addWidget(toggle, 0, Qt::AlignVCenter);
        } else {
            auto* setup = new ui::ui_pill_button(QStringLiteral("Set Up"), row);
            connect(setup, &QAbstractButton::clicked, row, [this, ext_id] {
                std::function<void(const QString&)> callback = on_setup_requested;
                close();
                if (callback) {
                    callback(ext_id);
                }
            });
            row_layout->addWidget(setup, 0, Qt::AlignVCenter);
        }

        m_rows_layout->addWidget(row);
    }

    m_rows_layout->addStretch(1);
}

} // namespace sheen
