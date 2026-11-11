#include "sheet.h"
#include "chrome.h"

#include <QCheckBox>
#include <QEvent>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

static constexpr int PROMPT_WIDTH = 460;
static constexpr int CHECKLIST_WIDTH = 520;
static constexpr int PANEL_MARGIN_X = 24;
static constexpr int PANEL_MARGIN_TOP = 22;
static constexpr int PANEL_MARGIN_BOTTOM = 20;
static constexpr int TITLE_BODY_GAP = 6;
static constexpr int BODY_BUTTONS_GAP = 20;
static constexpr int BUTTON_GAP = 8;
static constexpr int WINDOW_INSET = 24;
static constexpr int PANEL_TOP_MIN = 70;
static constexpr int PANEL_TOP_MAX = 150;
static constexpr int SHADOW_LAYERS = 8;
static constexpr int SHADOW_OFFSET_Y = 6;
static constexpr qreal SCRIM_ALPHA = 0.46;
static constexpr qreal SHADOW_LAYER_ALPHA = 0.05;
static constexpr qreal HALF_PIXEL = 0.5;
static constexpr qreal FOCUS_RING_ALPHA = 0.22;
static constexpr qreal FOCUS_RING_WIDTH = 2;
static constexpr float TITLE_SIZE = 15.5f;
static constexpr int ROW_HEIGHT = 44;
static constexpr int ROW_PADDING_X = 10;
static constexpr int LIST_PADDING = 4;
static constexpr int CHECK_SIZE = 16;
static constexpr qreal CHECKED_FILL_ALPHA = 0.11;
static constexpr qreal CHECK_STROKE = 1.5;
static constexpr int DOT_SIZE = 8;
static constexpr int CHECK_GAP = 12;

namespace {

/* The sheet's panel, opaque menu glass with the rim edge of a popover */
class sheet_panel : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(STLXTHEME_RIM), 1));
        painter.setBrush(token(STLXTHEME_MENU_BG));
        painter.drawRoundedRect(edge, STLXTHEME_WINDOW_RADIUS - HALF_PIXEL, STLXTHEME_WINDOW_RADIUS - HALF_PIXEL);
    }
};

/* The card the checklist's rows sit in, a step darker than the panel */
class checklist_frame : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(STLXTHEME_CARD_LINE), 1));
        painter.setBrush(token(STLXTHEME_CARD));
        painter.drawRoundedRect(edge, STLXTHEME_CARD_RADIUS - HALF_PIXEL, STLXTHEME_CARD_RADIUS - HALF_PIXEL);
    }
};

/* A checklist row: its check box, the file's name over its place, and the unsaved dot. A click anywhere toggles it */
class checklist_row : public QWidget {
public:
    checklist_row(const unsaved_entry& entry, bool is_last, QWidget* parent)
        : QWidget(parent), m_check(new QCheckBox(this)), m_is_last(is_last) {
        setFixedHeight(ROW_HEIGHT);
        m_check->setChecked(true);
        m_check->setStyleSheet(QStringLiteral("QCheckBox::indicator { width: %1px; height: %1px; border: none; "
                                              "background: transparent; image: none; }")
                                   .arg(CHECK_SIZE));
        connect(m_check, &QCheckBox::toggled, this, [this] { update(); });

        auto* name = new QLabel(entry.name, this);
        name->setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::DemiBold));
        auto* place = new QLabel(entry.place, this);
        place->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
        place->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_DIM).name()));

        auto* text = new QVBoxLayout;
        text->setSpacing(0);
        text->addWidget(name);
        text->addWidget(place);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(ROW_PADDING_X, 0, ROW_PADDING_X + DOT_SIZE + ROW_PADDING_X, 0);
        layout->setSpacing(CHECK_GAP);
        layout->addWidget(m_check);
        layout->addLayout(text, 1);
        m_check->installEventFilter(this);
    }

    QCheckBox* check() const { return m_check; }

protected:
    // The check box takes the focus and the row draws its ring, since the box alone is too small to show one
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_check && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
            update();
        }

        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(STLXTHEME_ACCENT));
        painter.drawEllipse(QPointF(width() - ROW_PADDING_X - DOT_SIZE / 2.0, height() / 2.0), DOT_SIZE / 2.0,
                            DOT_SIZE / 2.0);
        if (!m_is_last) {
            painter.fillRect(QRect(ROW_PADDING_X, height() - 1, width() - 2 * ROW_PADDING_X, 1),
                             token(STLXTHEME_LINE_SUBTLE));
        }

        // The row draws its box, recessed when clear and lit with a cream check when set
        QRectF box(0, 0, CHECK_SIZE, CHECK_SIZE);
        box.moveCenter(QRectF(m_check->geometry()).center());
        bool is_checked = m_check->isChecked();
        QColor fill = token(is_checked ? STLXTHEME_TEXT : STLXTHEME_RECESS);
        if (is_checked) {
            fill.setAlphaF(CHECKED_FILL_ALPHA);
        }

        painter.setPen(QPen(token(is_checked ? STLXTHEME_LINE_STRONG : STLXTHEME_LINE), 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(box.adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL), STLXTHEME_INNER_RADIUS,
                                STLXTHEME_INNER_RADIUS);
        if (is_checked) {
            QPainterPath check;
            check.moveTo(box.left() + 4.5, box.top() + 8.5);
            check.lineTo(box.left() + 7, box.top() + 11);
            check.lineTo(box.left() + 11.5, box.top() + 5.5);

            painter.setPen(QPen(token(STLXTHEME_TEXT), CHECK_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(check);
        }

        if (m_check->hasFocus()) {
            QColor ring = token(STLXTHEME_TEXT);
            ring.setAlphaF(FOCUS_RING_ALPHA);
            painter.setPen(QPen(ring, FOCUS_RING_WIDTH));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), STLXTHEME_CONTROL_RADIUS,
                                    STLXTHEME_CONTROL_RADIUS);
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override {
        m_check->toggle();
    }

private:
    QCheckBox* m_check;
    bool m_is_last;
};

} // namespace

static QLabel* make_title(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFont(ui_font(TITLE_SIZE, QFont::DemiBold));
    label->setWordWrap(true);
    return label;
}

static QLabel* make_body(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFont(ui_font(STLXTHEME_BODY_SIZE));
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_SECONDARY).name()));
    return label;
}

static QVBoxLayout* make_panel_layout(QWidget* panel) {
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(PANEL_MARGIN_X, PANEL_MARGIN_TOP, PANEL_MARGIN_X, PANEL_MARGIN_BOTTOM);
    layout->setSpacing(TITLE_BODY_GAP);
    return layout;
}

sheet::sheet(QWidget* window, int panel_width)
    : QWidget(window), m_panel(new sheet_panel(this)), m_panel_width(panel_width) {
    hide();
    window->installEventFilter(this);
}

sheet::~sheet() {
    parentWidget()->removeEventFilter(this);
}

int sheet::run() {
    place();
    show();
    raise();
    QWidget* focus = m_focus_target ? m_focus_target : m_panel->nextInFocusChain();
    focus->setFocus(Qt::OtherFocusReason);

    QEventLoop loop;
    m_loop = &loop;
    loop.exec(QEventLoop::DialogExec);
    m_loop = nullptr;

    hide();
    return m_result;
}

void sheet::finish(int result) {
    m_result = result;
    if (m_loop) {
        m_loop->quit();
    }
}

// The scrim, and the panel's shadow as rounded rings fading outward from its edge
void sheet::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QColor scrim = token(STLXTHEME_INK);
    scrim.setAlphaF(SCRIM_ALPHA);
    painter.fillRect(rect(), scrim);

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    QColor shadow = token(STLXTHEME_INK);
    shadow.setAlphaF(SHADOW_LAYER_ALPHA);
    painter.setPen(QPen(shadow, 1));

    QRectF edge = QRectF(m_panel->geometry()).translated(0, SHADOW_OFFSET_Y);
    for (int layer = 1; layer <= SHADOW_LAYERS; layer++) {
        QRectF ring = edge.adjusted(-layer + HALF_PIXEL, -layer + HALF_PIXEL, layer - HALF_PIXEL, layer - HALF_PIXEL);
        painter.drawRoundedRect(ring, STLXTHEME_WINDOW_RADIUS + layer, STLXTHEME_WINDOW_RADIUS + layer);
    }
}

void sheet::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        cancel();
        return;
    }

    QWidget::keyPressEvent(event);
}

bool sheet::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        place();
    }

    return QWidget::eventFilter(watched, event);
}

// Tab and Shift Tab move between the panel's controls and wrap, never reaching the window behind the scrim
bool sheet::focusNextPrevChild(bool next) {
    QList<QWidget*> controls;
    for (QWidget* widget : m_panel->findChildren<QWidget*>()) {
        if (widget->isVisibleTo(m_panel) && widget->isEnabled() && (widget->focusPolicy() & Qt::TabFocus)) {
            controls.append(widget);
        }
    }

    if (controls.isEmpty()) {
        return true;
    }

    int index = controls.indexOf(focusWidget());
    int count = static_cast<int>(controls.size());
    controls.at(((index + (next ? 1 : -1)) % count + count) % count)->setFocus(Qt::TabFocusReason);
    return true;
}

void sheet::cancel() {
    finish(0);
}

void sheet::set_panel_height(int height) {
    m_panel_height = height;
    if (isVisible()) {
        place();
    }
}

// Centered across the window, 70 to 150 px from its top, and never wider or taller than the window allows
void sheet::place() {
    QWidget* window = parentWidget();
    setGeometry(window->rect());

    int width = qMin(m_panel_width, window->width() - 2 * WINDOW_INSET);
    int wanted = m_panel_height > 0 ? m_panel_height : m_panel->layout()->heightForWidth(width);
    if (wanted <= 0) {
        wanted = m_panel->sizeHint().height();
    }

    int height = qMin(wanted, window->height() - 2 * WINDOW_INSET);
    int top = qBound(PANEL_TOP_MIN, (window->height() - height) / 3, PANEL_TOP_MAX);
    top = qMin(top, window->height() - height - WINDOW_INSET);
    m_panel->setGeometry((window->width() - width) / 2, qMax(WINDOW_INSET, top), width, height);
    update();
}

prompt_sheet::prompt_sheet(QWidget* window, const QString& title, const QString& body, const QString& primary,
                           const QString& destructive, bool is_primary_amber)
    : sheet(window, PROMPT_WIDTH) {
    QVBoxLayout* layout = make_panel_layout(panel());
    layout->addWidget(make_title(title, panel()));
    if (!body.isEmpty()) {
        layout->addWidget(make_body(body, panel()));
    }

    layout->addSpacing(BODY_BUTTONS_GAP);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(BUTTON_GAP);
    if (!destructive.isEmpty()) {
        m_destructive = new QPushButton(destructive, panel());
        connect(m_destructive, &QPushButton::clicked, this,
                [this] { finish(static_cast<int>(prompt_answer::DESTRUCTIVE)); });
        buttons->addWidget(m_destructive);
    }

    buttons->addStretch(1);
    QPushButton* cancel_button = new QPushButton(QStringLiteral("Cancel"), panel());
    connect(cancel_button, &QPushButton::clicked, this, [this] { finish(static_cast<int>(prompt_answer::CANCEL)); });
    buttons->addWidget(cancel_button);

    QPushButton* primary_button = new QPushButton(primary, panel());
    primary_button->setDefault(is_primary_amber);
    connect(primary_button, &QPushButton::clicked, this, [this] { finish(static_cast<int>(prompt_answer::PRIMARY)); });
    buttons->addWidget(primary_button);
    layout->addLayout(buttons);

    set_focus_target(primary_button);
}

prompt_answer prompt_sheet::ask() {
    return static_cast<prompt_answer>(run());
}

// Enter answers with the primary wherever focus sits, so a focused Don't Save is never one key from losing work
void prompt_sheet::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        finish(static_cast<int>(prompt_answer::PRIMARY));
        return;
    }

    if (m_destructive && event->key() == Qt::Key_D && event->modifiers() == Qt::ControlModifier) {
        finish(static_cast<int>(prompt_answer::DESTRUCTIVE));
        return;
    }

    sheet::keyPressEvent(event);
}

checklist_sheet::checklist_sheet(QWidget* window, const QString& title, const QString& body,
                                 const QList<unsaved_entry>& entries)
    : sheet(window, CHECKLIST_WIDTH), m_checked(entries.size(), true) {
    QVBoxLayout* layout = make_panel_layout(panel());
    layout->addWidget(make_title(title, panel()));
    layout->addWidget(make_body(body, panel()));
    layout->addSpacing(BODY_BUTTONS_GAP / 2);

    auto* list = new checklist_frame(panel());
    auto* rows = new QVBoxLayout(list);
    rows->setContentsMargins(LIST_PADDING, LIST_PADDING, LIST_PADDING, LIST_PADDING);
    rows->setSpacing(0);
    for (int index = 0; index < entries.size(); index++) {
        auto* row = new checklist_row(entries.at(index), index + 1 == entries.size(), list);
        connect(row->check(), &QCheckBox::toggled, this, [this, index](bool is_checked) {
            m_checked[index] = is_checked;
            refresh_primary();
        });
        rows->addWidget(row);
    }

    layout->addWidget(list);
    layout->addSpacing(BODY_BUTTONS_GAP);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(BUTTON_GAP);
    QPushButton* discard = new QPushButton(QStringLiteral("Discard All"), panel());
    connect(discard, &QPushButton::clicked, this, [this] { finish(static_cast<int>(prompt_answer::DESTRUCTIVE)); });
    buttons->addWidget(discard);
    buttons->addStretch(1);

    QPushButton* cancel_button = new QPushButton(QStringLiteral("Cancel"), panel());
    connect(cancel_button, &QPushButton::clicked, this, [this] { finish(static_cast<int>(prompt_answer::CANCEL)); });
    buttons->addWidget(cancel_button);

    m_primary = new QPushButton(QString(), panel());
    m_primary->setDefault(true);
    connect(m_primary, &QPushButton::clicked, this, [this] { finish(static_cast<int>(prompt_answer::PRIMARY)); });
    buttons->addWidget(m_primary);
    layout->addLayout(buttons);

    refresh_primary();
    set_focus_target(m_primary);
}

prompt_answer checklist_sheet::ask(QList<bool>* checked) {
    prompt_answer answer = static_cast<prompt_answer>(run());
    *checked = m_checked;
    return answer;
}

void checklist_sheet::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_primary->isEnabled()) {
            finish(static_cast<int>(prompt_answer::PRIMARY));
        }

        return;
    }

    if (event->key() == Qt::Key_D && event->modifiers() == Qt::ControlModifier) {
        finish(static_cast<int>(prompt_answer::DESTRUCTIVE));
        return;
    }

    sheet::keyPressEvent(event);
}

// The primary names how many it saves, and steps back when nothing is checked
void checklist_sheet::refresh_primary() {
    qsizetype count = m_checked.count(true);
    m_primary->setText(count == m_checked.size() ? QStringLiteral("Save All") : QStringLiteral("Save %1").arg(count));
    m_primary->setEnabled(count > 0);
}
