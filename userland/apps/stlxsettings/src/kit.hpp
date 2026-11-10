#ifndef STLXSETTINGS_KIT_HPP
#define STLXSETTINGS_KIT_HPP

/* The drawn components every Settings page shares, painted from the libstlxtheme tokens. Every surface but the
 * accent fill is a translucent overlay on the window background, so it shows glass wherever the window does. */
#include <QAbstractButton>
#include <QFont>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <functional>

class QHBoxLayout;
class QLabel;
class QPainter;
class QVBoxLayout;

/* The glyphs Settings draws, each on a 16 by 16 grid that `paint_glyph` scales to its box and fills with one color */
enum class glyph {
    WALLPAPER,
    DOCK,
    DISPLAY,
    KEYBOARD,
    POWER,
    LOCK,
    MORE,
    PLUS,
    PROGRAM,
    LOGO,
};

/* `LOGO` must stay last, since it ends the count */
constexpr int GLYPH_COUNT = static_cast<int>(glyph::LOGO) + 1;

/* The pane's margins and the line heights of body and caption text */
constexpr int PANE_MARGIN_X = 24;
constexpr int PANE_MARGIN_TOP = 20;
constexpr int PANE_MARGIN_BOTTOM = 24;
constexpr int BODY_LINE_HEIGHT = 20;
constexpr int CAPTION_LINE_HEIGHT = 16;

QColor theme_color(uint32_t argb);

/* Plex Sans (`ui_font`) or Plex Mono (`mono_font`) at an em size in pixels, half pixels included */
QFont ui_font(float em_px, QFont::Weight weight);
QFont mono_font(float em_px);

/* The font of section labels and group heads: caption size, semibold, tracked out. Callers must pass capitals */
QFont caps_font();

void paint_glyph(QPainter& painter, glyph shape, const QRectF& box, const QColor& color);

/* A single line of text in one font and color */
QLabel* make_text(const QString& text, const QFont& font, uint32_t color);

/* Rich text that sets a path or key apart in Plex Mono, for hints that name one */
QString code_text(const QString& text);

/* A section's caps title above its card, and at the right the config section its rows write, if any */
class section_head : public QWidget {
public:
    section_head(const QString& title, const QString& config_section);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_title;
    QString m_config_section;
};

/* A rounded panel of rows, one hairline between neighbors added with `add_row` */
class card : public QWidget {
public:
    card();

    void add_row(QWidget* row);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVBoxLayout* m_layout;
};

/* A setting's label over the config key it writes, in Plex Mono */
class setting_label : public QWidget {
public:
    setting_label(const QString& label, const QString& key);

    void set_key(const QString& key);

private:
    QLabel* m_key;
};

/* A card row: a setting's label and key, its controls at the right */
class setting_row : public QWidget {
public:
    setting_row(const QString& label, const QString& key);

    void add_control(QWidget* control);

    void set_key(const QString& key) { m_label->set_key(key); }
    QHBoxLayout* controls() const { return m_layout; }

private:
    setting_label* m_label;
    QHBoxLayout* m_layout;
};

/* A push button that runs `on_click` when clicked, with an optional glyph before its text. An enabled primary
 * button fills with the accent, which marks a pending change, and a small one fits list footers. */
class push_button : public QAbstractButton {
public:
    explicit push_button(const QString& text);

    std::function<void()> on_click;

    void set_glyph(glyph shape);
    void set_primary(bool is_primary);
    void set_small(bool is_small);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int glyph_size() const;
    int glyph_width() const;

    glyph m_glyph = glyph::PLUS;
    bool m_has_glyph = false;
    bool m_is_primary = false;
    bool m_is_small = false;
};

/* Paints the mark a segment shows before its label into `box`. `color` is the label's color, which a mark may use */
using mark_painter = std::function<void(QPainter& painter, const QRectF& box, int index, const QColor& color)>;

/* Choices on a recessed track, exactly one of them chosen. The chosen one is filled in cream, never amber */
class segmented : public QWidget {
public:
    segmented(const QStringList& labels, int chosen, mark_painter paint_mark);

    std::function<void(int)> on_choose;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    qreal segment_width(int index) const;
    QRectF segment_rect(int index) const;
    int segment_at(const QPointF& position) const;

    QStringList m_labels;
    mark_painter m_paint_mark;
    int m_chosen;
    int m_hovered = -1;
};

/* An on and off switch: a cream track with a dark knob when on. Never amber, since amber marks a pending change */
class toggle : public QWidget {
public:
    explicit toggle(bool is_on);

    std::function<void(bool)> on_toggle;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    bool m_is_on;
};

/* A whole number from `minimum` to `maximum`, picked in steps of `step` on a recessed track with `tick_count` ticks
 * spaced evenly along it. `minimum` must be below `maximum`, `step` above 0 and `tick_count` at least 2. */
class slider : public QWidget {
public:
    slider(int minimum, int maximum, int step, int value, int tick_count);

    std::function<void(int)> on_change;

    /* Moves the knob to `value`, kept within the range, without calling `on_change` */
    void set_value(int value);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    qreal x_of(int value) const;
    void move_to(qreal x);

    int m_minimum;
    int m_maximum;
    int m_step;
    int m_value;
    int m_tick_count;
    bool m_is_dragging = false;
};

/* A value in Plex Mono with its unit in the caption size after it, right aligned */
class value_label : public QWidget {
public:
    value_label(const QString& value, const QString& unit);

    void set_value(const QString& value);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_value;
    QString m_unit;
};

/* A lock beside a small Plex Mono label in a ring, marking an entry a drop-in file owns */
class chip : public QWidget {
public:
    explicit chip(const QString& text);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_text;
};

/* The square button that opens a list entry's menu */
class more_button : public QWidget {
public:
    more_button();

    std::function<void()> on_click;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
};

/* One entry of a list editor: an icon (the program glyph for a null one), the name over its path and arguments,
 * then trailing widgets. A locked (drop-in) entry shows its name in secondary text and does not light on hover */
class list_row : public QWidget {
public:
    list_row(const QString& name, const QString& path, const QString& args);

    std::function<void()> on_click;

    void set_text(const QString& name, const QString& path, const QString& args);
    void set_icon(const QPixmap& icon);
    void set_locked(bool is_locked);
    void set_open(bool is_open);
    void add_trailing(QWidget* widget);

    bool is_lit() const { return m_is_open || (!m_is_locked && underMouse()); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString m_name;
    QString m_path;
    QString m_args;
    QPixmap m_icon;
    bool m_is_locked = false;
    bool m_is_open = false;
    QHBoxLayout* m_layout;
};

/* A list editor's rows and the editors open under them, a hairline between two neighboring rows when neither
 * is lit, over a footer with the list's add button and a hint */
class list_box : public QWidget {
public:
    list_box();

    void add_row(list_row* row);
    void add_entry(QWidget* entry);
    void set_footer(push_button* add, const QString& hint);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVBoxLayout* m_entries;
    QList<list_row*> m_rows;
    QWidget* m_footer = nullptr;
};

#endif /* STLXSETTINGS_KIT_HPP */
