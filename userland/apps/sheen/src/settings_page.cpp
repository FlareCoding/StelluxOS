#include "settings_page.h"
#include "actions.h"
#include "chrome.h"
#include "encoding.h"
#include "field_frame.h"
#include "keycaps.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

static constexpr int RAIL_WIDTH = 184;
static constexpr int RAIL_PADDING = 8;
static constexpr int PAGE_MARGIN_X = 32;
static constexpr int PAGE_MARGIN_TOP = 24;
static constexpr int HEADER_GAP = 20;
static constexpr int GROUP_GAP = 18;
static constexpr int CAPTION_GAP = 6;
static constexpr int ROW_PADDING_X = 14;
static constexpr int ROW_PADDING_Y = 9;
static constexpr int ROW_LINE_GAP = 1;
static constexpr int MARKER_INSET = 10;
static constexpr int TOGGLE_WIDTH = 34;
static constexpr int TOGGLE_HEIGHT = 20;
static constexpr qreal TOGGLE_KNOB_INSET = 3;
static constexpr int CONTROL_HEIGHT = 28;
static constexpr int CONTROL_BORDER = 1;
static constexpr int SEGMENT_PADDING_X = 11;
static constexpr int STEP_WIDTH = 26;
static constexpr int VALUE_WIDTH = 56;
static constexpr int UNIT_GAP = 8;
static constexpr int ENCODING_WIDTH = 180;
static constexpr int FOOTER_HEIGHT = 52;
static constexpr int FOOTER_GAP = 8;
static constexpr int SHORTCUT_ROW_HEIGHT = 36;
static constexpr int KEYS_OR_GAP = 8;
static constexpr int FILTER_WIDTH = 260;
static constexpr qreal CAPTION_LETTER_SPACING = 106; // percent, 0.06 em between letters
static constexpr qreal HALF_PIXEL = 0.5;

struct page_entry {
    const char* name;
    const char* subtitle;
};

static constexpr page_entry PAGES[] = {
    { "Editor", "Type, indentation and the caret, for every tab." },
    { "Code", "How a tab in Code mode starts, before View changes it." },
    { "Text", "How a tab in Text mode starts, before View changes it." },
    { "Files", "New files, and the size from which a file opens plain." },
};

static QLabel* make_label(const QString& text, const QFont& font, uint32_t color, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: %1").arg(token(color).name()));
    return label;
}

static QFont caption_font() {
    QFont font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold);
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::PercentageSpacing, CAPTION_LETTER_SPACING);
    return font;
}

// The menu bar's menu that holds an action: File holds the palette and Settings, and Go the keys that pick a tab
static menu menu_bar_place(const command_entry& entry) {
    if (entry.place != menu::NONE) {
        return entry.place;
    }

    bool is_file = entry.id == command::COMMAND_PALETTE || entry.id == command::SETTINGS;
    return is_file ? menu::FILE : menu::GO;
}

// Each shortcut's keys as the menus name them. A symbol as the second key is the first key as some layouts report it
// with Shift held, such as Ctrl Shift ? for Ctrl Shift /, so it is left out
static QList<QStringList> entry_keys(const command_entry& entry) {
    QList<QStringList> keys;
    for (const char* shortcut : { entry.shortcut, entry.alternate }) {
        QKeySequence sequence(QString::fromLatin1(shortcut), QKeySequence::PortableText);
        QStringList names = shortcut_keys(sequence.toString(QKeySequence::NativeText));
        if (names.isEmpty()) {
            continue;
        }

        const QString& key = names.last();
        bool is_layout_twin = !keys.isEmpty() && key.size() == 1 && !key.at(0).isLetterOrNumber();
        if (!is_layout_twin) {
            keys.append(names);
        }
    }

    return keys;
}

namespace {

/* A switch for a key that is true or false, filled in cream while on */
class toggle_switch : public QAbstractButton {
public:
    explicit toggle_switch(QWidget* parent) : QAbstractButton(parent) {
        setCheckable(true);
        setFixedSize(TOGGLE_WIDTH, TOGGLE_HEIGHT);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF track = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(isChecked() ? STLXTHEME_TEXT : STLXTHEME_LINE), 1));
        painter.setBrush(token(isChecked() ? STLXTHEME_TEXT : STLXTHEME_RECESS));
        painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);

        qreal knob = height() - 2 * TOGGLE_KNOB_INSET;
        qreal left = isChecked() ? width() - TOGGLE_KNOB_INSET - knob : TOGGLE_KNOB_INSET;
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(isChecked() ? STLXTHEME_OPAQUE_POCKET : STLXTHEME_TEXT_DIM));
        painter.drawEllipse(QRectF(left, TOGGLE_KNOB_INSET, knob, knob));
    }
};

/* One choice of a segment, raised with a ring while chosen */
class segment_button : public QToolButton {
public:
    segment_button(const QString& label, QWidget* parent) : QToolButton(parent) {
        setText(label);
        setCheckable(true);
        setFocusPolicy(Qt::NoFocus);
        setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium));
        int width = fontMetrics().horizontalAdvance(label) + 2 * SEGMENT_PADDING_X;
        setFixedSize(width, CONTROL_HEIGHT - 2 * CONTROL_BORDER);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (isChecked()) {
            QRectF face = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
            painter.setPen(QPen(token(STLXTHEME_LINE_ON), 1));
            painter.setBrush(token(STLXTHEME_SURFACE_ON));
            painter.drawRoundedRect(face, STLXTHEME_MENU_ITEM_RADIUS, STLXTHEME_MENU_ITEM_RADIUS);
        }

        painter.setPen(token(isChecked() ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY));
        painter.drawText(rect(), Qt::AlignCenter, text());
    }
};

/* The recessed box a segment's choices or a stepper's parts sit in */
class control_box : public QWidget {
public:
    explicit control_box(QWidget* parent) : QWidget(parent), m_layout(new QHBoxLayout(this)) {
        m_layout->setContentsMargins(CONTROL_BORDER, CONTROL_BORDER, CONTROL_BORDER, CONTROL_BORDER);
        m_layout->setSpacing(0);
        setFixedHeight(CONTROL_HEIGHT);
    }

    void add(QWidget* part) {
        m_layout->addWidget(part);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(STLXTHEME_LINE), CONTROL_BORDER));
        painter.setBrush(token(STLXTHEME_RECESS));
        painter.drawRoundedRect(edge, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL);
    }

private:
    QHBoxLayout* m_layout;
};

/* The card a group's rows sit in, a hairline between each two */
class group_card : public QWidget {
public:
    explicit group_card(QWidget* parent) : QWidget(parent), m_layout(new QVBoxLayout(this)) {
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(ROW_LINE_GAP);
    }

    void add(QWidget* row) {
        m_layout->addWidget(row);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(STLXTHEME_LINE_SUBTLE), 1));
        painter.setBrush(token(STLXTHEME_SURFACE));
        painter.drawRoundedRect(edge, STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);

        // A filter hides rows, so the hairlines go above each shown row but the first
        bool is_first = true;
        for (int index = 0; index < m_layout->count(); index++) {
            QWidget* row = m_layout->itemAt(index)->widget();
            if (row->isHidden()) {
                continue;
            }

            if (!is_first) {
                int top = row->geometry().top() - ROW_LINE_GAP;
                painter.fillRect(QRect(0, top, width(), ROW_LINE_GAP), token(STLXTHEME_LINE_SUBTLE));
            }

            is_first = false;
        }
    }

private:
    QVBoxLayout* m_layout;
};

} // namespace

// A page's margins and its column of titles, groups and rows
static QVBoxLayout* make_page_layout(QWidget* content) {
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(PAGE_MARGIN_X, PAGE_MARGIN_TOP, PAGE_MARGIN_X, PAGE_MARGIN_TOP);
    layout->setSpacing(0);
    return layout;
}

static QVBoxLayout* make_titles(const QString& name, const QString& subtitle, QWidget* content) {
    auto* titles = new QVBoxLayout;
    titles->addWidget(make_label(name, ui_font(STLXTHEME_PAGE_TITLE_SIZE, QFont::DemiBold), STLXTHEME_TEXT, content));
    titles->addWidget(make_label(subtitle, ui_font(STLXTHEME_BODY_SIZE), STLXTHEME_TEXT_DIM, content));
    return titles;
}

// A group's caption over its card, the caption holding the gap above it so a hidden group leaves no gap behind
static std::pair<QLabel*, group_card*> add_group(QVBoxLayout* layout, const QString& name, bool is_first,
                                                 QWidget* content) {
    QLabel* caption = make_label(name, caption_font(), STLXTHEME_TEXT_DIM, content);
    caption->setContentsMargins(0, is_first ? HEADER_GAP : GROUP_GAP, 0, CAPTION_GAP);
    auto* card = new group_card(content);
    layout->addWidget(caption);
    layout->addWidget(card);
    return { caption, card };
}

// The page scrolls over the tab's own backdrop, so neither it nor the viewport fills itself
static QWidget* make_scroll(QWidget* content) {
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    scroll->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    return scroll;
}

/* One key's row: its label, note and `key = value` line beside its control, marked in amber while changed */
class setting_row : public QWidget {
public:
    setting_row(const setting_field& field, const QString& value, QWidget* parent)
        : QWidget(parent), m_field(field), m_saved(value), m_value(value) {
        auto* words = new QVBoxLayout;
        words->setSpacing(0);
        words->addWidget(make_label(QString::fromLatin1(field.label), ui_font(STLXTHEME_BODY_SIZE, QFont::Medium),
                                    STLXTHEME_TEXT, this));
        if (field.note[0]) {
            words->addWidget(make_label(QString::fromLatin1(field.note), ui_font(STLXTHEME_CAPTION_SIZE),
                                        STLXTHEME_TEXT_DIM, this));
        }

        m_key_line = make_label(QString(), mono_font(STLXTHEME_CAPTION_SIZE), STLXTHEME_TEXT_DIM, this);
        words->addWidget(m_key_line);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(ROW_PADDING_X, ROW_PADDING_Y, ROW_PADDING_X, ROW_PADDING_Y);
        layout->addLayout(words, 1);
        layout->addWidget(make_control(), 0, Qt::AlignVCenter);
        if (field.unit[0]) {
            layout->addSpacing(UNIT_GAP);
            layout->addWidget(make_label(QString::fromLatin1(field.unit), ui_font(STLXTHEME_CAPTION_SIZE),
                                         STLXTHEME_TEXT_DIM, this));
        }

        show_value();
    }

    QString key_path() const {
        return QStringLiteral("%1.%2").arg(QString::fromLatin1(m_field.section), QString::fromLatin1(m_field.key));
    }

    QString value() const {
        return m_value;
    }

    bool is_changed() const {
        return m_value != m_saved;
    }

    void reset(const QString& saved) {
        m_saved = saved;
        m_value = saved;
        show_value();
    }

    std::function<void()> on_changed;

protected:
    void paintEvent(QPaintEvent*) override {
        if (is_changed()) {
            QPainter painter(this);
            painter.fillRect(QRect(0, MARKER_INSET, STLXTHEME_MARKER_WIDTH, height() - 2 * MARKER_INSET),
                             token(STLXTHEME_ACCENT));
        }
    }

private:
    QWidget* make_control() {
        switch (m_field.control) {
            case setting_control::TOGGLE:
                return make_toggle();
            case setting_control::CHOICE:
                return make_choice();
            case setting_control::NUMBER:
                return make_stepper();
            case setting_control::ENCODING:
                return make_encoding_list();
        }

        return nullptr;
    }

    QWidget* make_toggle() {
        m_toggle = new toggle_switch(this);
        connect(m_toggle, &QAbstractButton::clicked, this, [this](bool is_on) {
            set_value(is_on ? QStringLiteral("true") : QStringLiteral("false"));
        });

        return m_toggle;
    }

    QWidget* make_choice() {
        auto* box = new control_box(this);
        QStringList values = QString::fromLatin1(m_field.choices).split(QLatin1Char(' '));
        QStringList labels = QString::fromLatin1(m_field.choice_labels).split(QLatin1Char(' '));
        for (qsizetype index = 0; index < values.size(); index++) {
            auto* button = new segment_button(labels.value(index), box);
            box->add(button);
            m_choices.append({ values.at(index), button });
            connect(button, &QAbstractButton::clicked, this, [this, value = values.at(index)] { set_value(value); });
        }

        return box;
    }

    QWidget* make_stepper() {
        auto* box = new control_box(this);
        auto* down = new QToolButton(box);
        auto* up = new QToolButton(box);

        m_number = make_label(QString(), mono_font(STLXTHEME_BODY_SIZE), STLXTHEME_TEXT, box);
        m_number->setAlignment(Qt::AlignCenter);
        m_number->setFixedWidth(VALUE_WIDTH);
        for (auto [button, glyph] : { std::pair { down, QStringLiteral("-") }, std::pair { up, QStringLiteral("+") } }) {
            button->setText(glyph);
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setFixedSize(STEP_WIDTH, CONTROL_HEIGHT - 2 * CONTROL_BORDER);
        }

        box->add(down);
        box->add(m_number);
        box->add(up);
        connect(down, &QToolButton::clicked, this, [this] { step(-1); });
        connect(up, &QToolButton::clicked, this, [this] { step(1); });
        return box;
    }

    QWidget* make_encoding_list() {
        m_encodings = new QComboBox(this);
        m_encodings->setFixedWidth(ENCODING_WIDTH);
        for (const QByteArray& name : available_encodings()) {
            m_encodings->addItem(encoding_label(name), QString::fromLatin1(name));
        }

        connect(m_encodings, &QComboBox::activated, this, [this](int index) {
            set_value(m_encodings->itemData(index).toString());
        });

        return m_encodings;
    }

    // A step stays in the key's range, and a value off the steps lands on the nearest one
    void step(int direction) {
        double value = m_value.toDouble() + direction * m_field.step;
        value = qBound(m_field.minimum, std::round(value / m_field.step) * m_field.step, m_field.maximum);
        set_value(QString::number(value));
    }

    void set_value(const QString& value) {
        m_value = value;
        show_value();
        if (on_changed) {
            on_changed();
        }
    }

    void show_value() {
        m_key_line->setText(QStringLiteral("%1 = %2").arg(QString::fromLatin1(m_field.key), m_value));
        if (m_toggle) {
            m_toggle->setChecked(m_value == QLatin1String("true"));
        }

        for (const auto& [value, button] : m_choices) {
            button->setChecked(value == m_value);
        }

        if (m_number) {
            m_number->setText(m_value);
        }

        if (m_encodings) {
            m_encodings->setCurrentIndex(m_encodings->findData(m_value));
        }

        update();
    }

    setting_field m_field;
    QString m_saved;
    QString m_value;
    QLabel* m_key_line = nullptr;
    toggle_switch* m_toggle = nullptr;
    QList<std::pair<QString, segment_button*>> m_choices;
    QLabel* m_number = nullptr;
    QComboBox* m_encodings = nullptr;
};

/* One action's row on the Shortcuts page: its name, and the keycaps of each shortcut that runs it */
class shortcut_row : public QWidget {
public:
    shortcut_row(const QString& group, const command_entry& entry, QWidget* parent)
        : QWidget(parent), m_label(QString::fromLatin1(entry.label)), m_keys(entry_keys(entry)) {
        setFixedHeight(SHORTCUT_ROW_HEIGHT);
        QString named = QStringLiteral("%1 %2").arg(group, m_label).remove(QStringLiteral("..."));
        m_words = named.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QStringList& keys : m_keys) {
            for (const QString& key : keys) {
                m_words.append(key.toLower());
            }
        }
    }

    // Each of `words` begins a word of the action's name, its menu or one of its keys
    bool matches(const QStringList& words) const {
        for (const QString& word : words) {
            auto begins = [&word](const QString& own) { return own.startsWith(word); };
            if (std::none_of(m_words.begin(), m_words.end(), begins)) {
                return false;
            }
        }

        return true;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium));
        painter.setPen(token(STLXTHEME_TEXT));
        painter.drawText(rect().adjusted(ROW_PADDING_X, 0, -ROW_PADDING_X, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         m_label);

        QString joint = QStringLiteral("or");
        QFont joint_font = ui_font(STLXTHEME_CAPTION_SIZE);
        int joint_width = QFontMetrics(joint_font).horizontalAdvance(joint);
        int right = width() - ROW_PADDING_X;
        for (qsizetype index = m_keys.size() - 1; index >= 0; index--) {
            right -= keycaps_width(m_keys.at(index));
            paint_keycaps(&painter, m_keys.at(index), right, height() / 2, token(STLXTHEME_TEXT_SECONDARY));
            if (index == 0) {
                break;
            }

            right -= KEYS_OR_GAP + joint_width;
            painter.setFont(joint_font);
            painter.setPen(token(STLXTHEME_TEXT_DIM));
            painter.drawText(QRect(right, 0, joint_width, height()), Qt::AlignCenter, joint);
            right -= KEYS_OR_GAP;
        }
    }

private:
    QString m_label;
    QList<QStringList> m_keys;
    QStringList m_words;
};

settings_page::settings_page(const sheen_settings& settings, QWidget* parent)
    : QWidget(parent), m_rail(new QListWidget(this)), m_pages(new QStackedWidget(this)), m_saved(settings) {
    m_rail->setFixedWidth(RAIL_WIDTH);
    m_rail->setFrameShape(QFrame::NoFrame);
    m_rail->setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium));
    m_rail->setStyleSheet(QStringLiteral("QListWidget { background: transparent; padding: %1px; }"
                                         "QListWidget::item { height: 32px; padding-left: 10px; color: %2; "
                                         "border-radius: %3px; }"
                                         "QListWidget::item:selected { background: %4; color: %5; }")
                              .arg(RAIL_PADDING)
                              .arg(token(STLXTHEME_TEXT_SECONDARY).name())
                              .arg(STLXTHEME_MENU_ITEM_RADIUS)
                              .arg(token(STLXTHEME_SURFACE_SELECTED).name(QColor::HexArgb))
                              .arg(token(STLXTHEME_TEXT).name()));

    QList<setting_field> fields = setting_fields();
    for (const page_entry& entry : PAGES) {
        QString name = QString::fromLatin1(entry.name);
        QList<setting_field> page_fields;
        for (const setting_field& field : fields) {
            if (name == QLatin1String(field.page)) {
                page_fields.append(field);
            }
        }

        m_rail->addItem(name);
        m_pages->addWidget(make_page(name, QString::fromLatin1(entry.subtitle), page_fields));
    }

    m_rail->addItem(QStringLiteral("Shortcuts"));
    m_pages->addWidget(make_shortcuts_page());

    auto* footer = new QWidget(this);
    footer->setFixedHeight(FOOTER_HEIGHT);
    m_footer_note = make_label(QString(), ui_font(STLXTHEME_CAPTION_SIZE), STLXTHEME_TEXT_DIM, footer);
    m_revert = new QPushButton(QStringLiteral("Revert"), footer);
    m_save = new QPushButton(QStringLiteral("Save"), footer);
    m_save->setDefault(true);

    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(PAGE_MARGIN_X, 0, PAGE_MARGIN_X, 0);
    footer_layout->setSpacing(FOOTER_GAP);
    footer_layout->addWidget(m_footer_note, 1);
    footer_layout->addWidget(m_revert);
    footer_layout->addWidget(m_save);

    auto* body = new QVBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_pages, 1);
    body->addWidget(footer);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_rail);
    layout->addLayout(body, 1);

    connect(m_rail, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_revert, &QPushButton::clicked, this, [this] { reset(m_saved); });
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (on_save) {
            on_save();
        }
    });

    m_rail->setCurrentRow(0);
    refresh_footer();
}

QWidget* settings_page::make_page(const QString& name, const QString& subtitle, const QList<setting_field>& fields) {
    auto* content = new QWidget;
    QVBoxLayout* layout = make_page_layout(content);

    auto* open_as_text = new QPushButton(QStringLiteral("Open as Text"), content);
    connect(open_as_text, &QPushButton::clicked, this, [this] {
        if (on_open_as_text) {
            on_open_as_text();
        }
    });

    auto* header = new QHBoxLayout;
    header->addLayout(make_titles(name, subtitle, content), 1);
    header->addWidget(make_label(QString::fromLatin1(SETTINGS_PATH), mono_font(STLXTHEME_CAPTION_SIZE),
                                 STLXTHEME_TEXT_DIM, content));
    header->addSpacing(FOOTER_GAP);
    header->addWidget(open_as_text);
    layout->addLayout(header);

    QString group;
    group_card* card = nullptr;
    for (const setting_field& field : fields) {
        if (group != QLatin1String(field.group)) {
            group = QString::fromLatin1(field.group);
            card = add_group(layout, group, !card, content).second;
        }

        QString value = setting_value(m_saved, QString::fromLatin1(field.section), QString::fromLatin1(field.key));
        auto* row = new setting_row(field, value, card);
        row->on_changed = [this] { refresh_footer(); };
        card->add(row);
        m_rows.append(row);
    }

    layout->addStretch(1);
    return make_scroll(content);
}

// Every action under the menu bar's menu that holds it, filtered as the field is typed in
QWidget* settings_page::make_shortcuts_page() {
    auto* content = new QWidget;
    QVBoxLayout* layout = make_page_layout(content);

    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(QStringLiteral("Filter by name or key"));
    auto* filter_field = new field_frame(m_filter, content);
    filter_field->setFixedWidth(FILTER_WIDTH);
    filter_field->show_magnifier();
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& query) { filter_shortcuts(query); });

    auto* header = new QHBoxLayout;
    header->addLayout(make_titles(QStringLiteral("Shortcuts"), QStringLiteral("Every action and the keys that run it."),
                                  content),
                      1);
    header->addWidget(filter_field);
    layout->addLayout(header);

    for (int place = 0; place < static_cast<int>(std::size(MENU_TITLES)); place++) {
        QString group = QString::fromLatin1(MENU_TITLES[place]);
        auto [caption, card] = add_group(layout, group, place == 0, content);
        m_shortcut_groups.append({ caption, card });
        for (const command_entry& entry : COMMANDS) {
            if (entry.kind != command_kind::SUBMENU && static_cast<int>(menu_bar_place(entry)) == place) {
                auto* row = new shortcut_row(group, entry, card);
                card->add(row);
                m_shortcut_rows.append(row);
            }
        }
    }

    m_no_match = make_label(QString(), ui_font(STLXTHEME_BODY_SIZE), STLXTHEME_TEXT_DIM, content);
    m_no_match->setContentsMargins(0, HEADER_GAP, 0, 0);
    m_no_match->hide();
    layout->addWidget(m_no_match);

    layout->addStretch(1);
    return make_scroll(content);
}

// A menu's group shows while any of its rows matches, and a note stands in when none does
void settings_page::filter_shortcuts(const QString& query) {
    static const QRegularExpression separators(QStringLiteral("[\\s+]+"));
    QStringList words = query.toLower().split(separators, Qt::SkipEmptyParts);
    for (shortcut_row* row : m_shortcut_rows) {
        row->setVisible(row->matches(words));
    }

    bool is_any_shown = false;
    for (const std::pair<QWidget*, QWidget*>& group : m_shortcut_groups) {
        QWidget* card = group.second;
        auto is_shown_in_card = [card](const shortcut_row* row) {
            return row->parentWidget() == card && !row->isHidden();
        };

        bool is_shown = std::any_of(m_shortcut_rows.begin(), m_shortcut_rows.end(), is_shown_in_card);
        group.first->setVisible(is_shown);
        card->setVisible(is_shown);
        is_any_shown = is_any_shown || is_shown;
    }

    m_no_match->setText(QStringLiteral("No action matches \"%1\"").arg(query.trimmed()));
    m_no_match->setVisible(!is_any_shown);
}

QHash<QString, QString> settings_page::changes() const {
    QHash<QString, QString> changed;
    for (const setting_row* row : m_rows) {
        if (row->is_changed()) {
            changed.insert(row->key_path(), row->value());
        }
    }

    return changed;
}

void settings_page::reset(const sheen_settings& settings) {
    m_saved = settings;
    for (setting_row* row : m_rows) {
        QString key_path = row->key_path();
        row->reset(setting_value(settings, key_path.section(QLatin1Char('.'), 0, 0),
                                 key_path.section(QLatin1Char('.'), 1)));
    }

    refresh_footer();
}

void settings_page::show_section(const QString& name) {
    QList<QListWidgetItem*> items = m_rail->findItems(name, Qt::MatchExactly);
    if (!items.isEmpty()) {
        m_rail->setCurrentItem(items.first());
    }

    if (name == QLatin1String("Shortcuts")) {
        m_filter->setFocus();
        m_filter->selectAll();
    }
}

void settings_page::refresh_footer() {
    qsizetype count = changes().size();
    QString note = count == 0   ? QStringLiteral("Changes apply to every tab once saved")
                 : count == 1   ? QStringLiteral("1 unsaved change, applies to every tab on save")
                                : QStringLiteral("%1 unsaved changes, apply to every tab on save").arg(count);
    m_footer_note->setText(note);
    m_revert->setEnabled(count > 0);

    // With nothing to save the amber Save is withdrawn, dim beside Revert, as Stellux Settings does
    m_save->setDefault(count > 0);
    m_save->setEnabled(count > 0);
    if (on_changed) {
        on_changed();
    }
}
