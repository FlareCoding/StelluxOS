#include "workspace/extension_setup_page.h"
#include "app/sheen_app.h"
#include "extensions/registry.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"
#include "workspace/dependency_installer.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace sheen {

static constexpr int PAGE_MAX_WIDTH = 760;
static constexpr int CARD_MIN_WIDTH = 420;
static constexpr int CARD_COLUMN_STRETCH = 100;
static constexpr int CARD_VERTICAL_SPACING = 14;
static constexpr int CARD_INNER_SPACING = 10;
static constexpr int PAGE_MARGIN_X = 32;
static constexpr int PAGE_MARGIN_Y = 24;
static constexpr int CARD_PADDING = 16;
static constexpr int LOG_HEIGHT = 180;
static constexpr float PAGE_HEADING_SIZE = 20.0f;
static constexpr float CARD_TITLE_SIZE = 15.0f;
static constexpr qreal HALF_PIXEL = 0.5;

namespace {

class setup_card : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF box = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(STLXTHEME_CARD_LINE), 1));
        painter.setBrush(token(STLXTHEME_CARD));
        painter.drawRoundedRect(box, STLXTHEME_CARD_RADIUS, STLXTHEME_CARD_RADIUS);
    }
};

} // namespace

extension_setup_page::extension_setup_page(sheen_app& app, const QString& focus_extension_id, QWidget* parent)
    : QWidget(parent), m_app(app), m_focus_extension_id(focus_extension_id) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    auto* content = new QWidget(scroll);
    content->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* outer = new QHBoxLayout(content);
    outer->setContentsMargins(PAGE_MARGIN_X, PAGE_MARGIN_Y, PAGE_MARGIN_X, PAGE_MARGIN_Y);
    outer->addStretch(1);

    m_cards_container = new QWidget(content);
    m_cards_container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_cards_container->setMinimumWidth(CARD_MIN_WIDTH);
    m_cards_container->setMaximumWidth(PAGE_MAX_WIDTH);

    m_cards_layout = new QVBoxLayout(m_cards_container);
    m_cards_layout->setContentsMargins(0, 0, 0, 0);
    m_cards_layout->setSpacing(CARD_VERTICAL_SPACING);
    outer->addWidget(m_cards_container, CARD_COLUMN_STRETCH);
    outer->addStretch(1);

    scroll->setWidget(content);
    root_layout->addWidget(scroll, 1);

    m_log = new ui::ui_log_panel({}, this);
    m_log->setFixedHeight(LOG_HEIGHT);
    m_log->hide();
    root_layout->addWidget(m_log);

    dependency_installer& installer = m_app.installer();
    if (!installer.log_history().isEmpty()) {
        m_log->show();
        m_log->append_bytes(installer.log_history());
    }

    installer.add_listener(
        this,
        [this](QByteArrayView bytes) {
            m_log->show();
            m_log->append_bytes(bytes);
        },
        [this] { refresh_state(); },
        [this](const QString& finished_id, bool succeeded, const QString& failure_message) {
            if (succeeded) {
                m_log->append_mark(QStringLiteral("Setup complete"), false);
                refresh_state();
                if (m_pending_enable_id == finished_id && on_extension_ready) {
                    m_pending_enable_id.clear();
                    on_extension_ready(finished_id);
                }

                return;
            }

            m_pending_enable_id.clear();
            m_log->show();
            m_log->append_mark(failure_message, true);
            refresh_state();
        });

    refresh_state();
}

extension_setup_page::~extension_setup_page() {
    m_app.installer().remove_listener(this);
}

void extension_setup_page::focus_extension(const QString& extension_id) {
    m_focus_extension_id = extension_id;
    refresh_state();
}

void extension_setup_page::start_install(const extension_descriptor& descriptor) {
    m_log->clear_log();
    m_log->show();
    m_pending_enable_id = QString::fromLatin1(descriptor.id);

    QString error;
    if (!m_app.installer().start_extension_install(descriptor, &error)) {
        m_pending_enable_id.clear();
        m_log->append_mark(error, true);
    }

    refresh_state();
}

void extension_setup_page::refresh_state() {
    while (QLayoutItem* item = m_cards_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }

        delete item;
    }

    auto* heading = new QLabel(QStringLiteral("Extension Setup"), m_cards_container);
    heading->setFont(ui_font(PAGE_HEADING_SIZE, QFont::DemiBold));
    heading->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(token(STLXTHEME_TEXT).name()));
    m_cards_layout->addWidget(heading);

    auto* subtitle = new QLabel(
        QStringLiteral("Install toolchains, runtimes, and SDK packages required by workspace extensions."),
        m_cards_container);
    subtitle->setFont(ui_font(STLXTHEME_BODY_SIZE));
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                .arg(token(STLXTHEME_TEXT_SECONDARY).name()));
    m_cards_layout->addWidget(subtitle);

    std::span<const extension_descriptor> extensions = registered_extensions();
    if (extensions.empty()) {
        auto* empty_card = new setup_card(m_cards_container);
        auto* card_layout = new QVBoxLayout(empty_card);
        card_layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);

        auto* note = new QLabel(
            QStringLiteral("No extensions require additional setup in this build. "
                           "Compiled extensions appear here with one-click package installation."),
            empty_card);
        note->setFont(ui_font(STLXTHEME_BODY_SIZE));
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                .arg(token(STLXTHEME_TEXT_DIM).name()));

        card_layout->addWidget(note);
        m_cards_layout->addWidget(empty_card);
        m_cards_layout->addStretch(1);
        return;
    }

    dependency_installer& installer = m_app.installer();
    for (const extension_descriptor& desc : extensions) {
        QString ext_id = QString::fromLatin1(desc.id);
        bool ready = are_dependencies_installed(desc);
        bool installing_this = installer.is_running() && installer.target_extension_id() == ext_id;

        auto* card = new setup_card(m_cards_container);
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
        card_layout->setSpacing(CARD_INNER_SPACING);

        auto* top_row = new QHBoxLayout;
        auto* title = new QLabel(QString::fromLatin1(desc.name), card);
        title->setFont(ui_font(CARD_TITLE_SIZE, QFont::DemiBold));
        title->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(token(STLXTHEME_TEXT).name()));
        top_row->addWidget(title);
        top_row->addStretch(1);

        if (installing_this) {
            auto* cancel_button = new ui::ui_pill_button(QStringLiteral("Cancel"), card);
            connect(cancel_button, &QAbstractButton::clicked, card, [this] { m_app.installer().cancel(); });
            top_row->addWidget(cancel_button);
        } else if (ready) {
            auto* badge = new QLabel(QStringLiteral("Ready"), card);
            badge->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
            badge->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                     .arg(token(STLXTHEME_ACCENT).name()));
            top_row->addWidget(badge);
        } else {
            bool had_error = installer.last_failed_extension_id() == ext_id;
            QString button_label = had_error ? QStringLiteral("Retry") : QStringLiteral("Install");
            auto* install_button = new ui::ui_pill_button(button_label, card);
            install_button->setEnabled(!installer.is_running());
            connect(install_button, &QAbstractButton::clicked, card, [this, desc] { start_install(desc); });
            top_row->addWidget(install_button);
        }

        card_layout->addLayout(top_row);

        auto* desc_label = new QLabel(QString::fromLatin1(desc.summary), card);
        desc_label->setFont(ui_font(STLXTHEME_BODY_SIZE));
        desc_label->setWordWrap(true);
        desc_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                      .arg(token(STLXTHEME_TEXT_SECONDARY).name()));
        card_layout->addWidget(desc_label);

        for (const dependency_group& group : desc.dependencies) {
            bool group_ready = is_dependency_group_installed(group);
            auto* group_row = new QHBoxLayout;
            auto* group_title = new QLabel(QString::fromLatin1(group.title), card);
            group_title->setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium));
            group_title->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                           .arg(token(STLXTHEME_TEXT).name()));
            group_row->addWidget(group_title);

            auto* group_state = new QLabel(group_ready ? QStringLiteral("Installed") : QStringLiteral("Missing"), card);
            group_state->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
            uint32_t state_tone = group_ready ? STLXTHEME_TEXT_DIM : STLXTHEME_ACCENT;
            group_state->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                           .arg(token(state_tone).name()));

            group_row->addStretch(1);
            group_row->addWidget(group_state);
            card_layout->addLayout(group_row);
        }

        if (installing_this && !installer.progress_label().isEmpty()) {
            int percent = installer.progress_percent();
            QString status = percent >= 0
                           ? QStringLiteral("%1 (%2%)").arg(installer.progress_label()).arg(percent)
                           : installer.progress_label();
            auto* progress_label = new QLabel(status, card);
            progress_label->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
            progress_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                              .arg(token(STLXTHEME_ACCENT).name()));
            card_layout->addWidget(progress_label);
        }

        if (installer.last_failed_extension_id() == ext_id && !installer.last_failure_message().isEmpty()) {
            auto* error_label = new QLabel(installer.last_failure_message(), card);
            error_label->setFont(ui_font(STLXTHEME_CAPTION_SIZE));
            error_label->setWordWrap(true);
            error_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                           .arg(token(STLXTHEME_DANGER).name()));
            card_layout->addWidget(error_label);
        }

        m_cards_layout->addWidget(card);
    }

    m_cards_layout->addStretch(1);
}

void extension_setup_page::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_OPAQUE_POCKET));
}

} // namespace sheen
