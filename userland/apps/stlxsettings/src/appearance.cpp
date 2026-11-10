#include "kit.hpp"
#include "settings.hpp"

#include <stlxtheme/theme.h>

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <iterator>

constexpr const char* WALLPAPER_DIR = "/etc/res/wallpapers";

constexpr const char* WALLPAPER_CARD = "Wallpaper";
constexpr const char* STYLE_CARD = "Style";
constexpr const char* IMAGE_LABEL = "Image";
constexpr const char* WALLPAPER_KEY = "wallpaper";
constexpr const char* GLASS_LABEL = "Glass";
constexpr const char* GLASS_KEY = "glass_level";
constexpr const char* TINT_LABEL = "Tint";
constexpr const char* TINT_KEY = "glass_tint";
constexpr const char* ACCENT_LABEL = "Accent";
constexpr const char* ACCENT_NAME = "Amber";

constexpr int PREVIEW_W = 185;
constexpr int PREVIEW_H = 104;
constexpr int PREVIEW_BAR_H = 4;
constexpr int PREVIEW_DOCK_W = 19;
constexpr int PREVIEW_DOCK_H = 5;
constexpr int PREVIEW_DOCK_BOTTOM = 2;
constexpr int PREVIEW_DOCK_RADIUS = 2;

/* The box wallpapers decode to cover: larger than the preview, and small enough that a JPEG decodes quickly */
constexpr int DECODE_W = 240;
constexpr int DECODE_H = 135;

constexpr int CHOICE_W = 112;
constexpr int CHOICE_H = 63;
constexpr int CHOICE_MARGIN = 4;
constexpr int CHOICE_NAME_GAP = 6;
constexpr int CHOICE_SPACING = 6;
constexpr int CHOICES_PADDING_X = 10;
constexpr int CHOICES_PADDING_Y = 8;

/* With the card's 1 px border the preview sits 4 px inside the card's edge, the card's radius less the thumbnail's, so
 * their corners are concentric. */
constexpr int WALLPAPER_CARD_PADDING = 3;

constexpr int DETAILS_GAP = 16;
constexpr int DETAILS_PADDING_RIGHT = 9;
constexpr int DETAILS_PADDING_Y = 4;
constexpr int DETAILS_SPACING = 6;
constexpr qint64 BYTES_PER_KB = 1024;

constexpr int CHIP_DOT_SIZE = 10;
constexpr int CHIP_GAP = 6;
constexpr int ACCENT_DOT_SIZE = 14;
constexpr int ACCENT_GAP = 10;
constexpr int SEGMENT_MARK_RADIUS = 3;

/* In `stlxconf_glass_level_t` order */
static const char* const GLASS_LEVEL_NAMES[] = { "Solid", "Subtle", "Glass", "Clear" };
static const qreal GLASS_MARK_FILLS[] = { 1.0, 0.66, 0.33, 0.0 };
static_assert(std::size(GLASS_MARK_FILLS) == std::size(GLASS_LEVEL_NAMES));

static const char* const TINT_NAMES[] = { "Neutral", "Warm" };
static const stlxconf_glass_tint_t TINT_VALUES[] = { STLXCONF_GLASS_TINT_NEUTRAL, STLXCONF_GLASS_TINT_WARM };
static const uint32_t TINT_MARK_COLORS[] = { STLXTHEME_TINT_SWATCH_NEUTRAL, STLXTHEME_TINT_SWATCH_WARM };
static_assert(std::size(TINT_VALUES) == std::size(TINT_NAMES));
static_assert(std::size(TINT_MARK_COLORS) == std::size(TINT_NAMES));

static QHash<QString, QImage> g_decoded_wallpapers;

static void paint_rounded_image(QPainter& painter, const QImage& image, const QRectF& box);
static QString hex_text(uint32_t color);

namespace {

/* The wallpaper as the desktop crops it for a 16:9 screen, with the bar and the dock drawn small on top */
class wallpaper_preview : public QWidget {
public:
    wallpaper_preview() {
        setFixedSize(PREVIEW_W, PREVIEW_H);
    }

    void set_image(const QImage& image) {
        m_image = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paint_rounded_image(painter, m_image, QRectF(rect()));
    }

private:
    QImage m_image;
};

/* One wallpaper file to pick, named under its picture, with a cream ring when chosen, else a brass one when hovered */
class wallpaper_choice : public QWidget {
public:
    wallpaper_choice(const QString& path, const QImage& image)
        : m_path(path), m_name(QFileInfo(path).completeBaseName()), m_image(image) {
        setFixedSize(CHOICE_W + 2 * CHOICE_MARGIN, CHOICE_MARGIN + CHOICE_H + CHOICE_NAME_GAP + CAPTION_LINE_HEIGHT);
        setAttribute(Qt::WA_Hover);
    }

    std::function<void(const QString&)> on_choose;

    const QString& path() const { return m_path; }

    void set_chosen(bool is_chosen) {
        m_is_chosen = is_chosen;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF picture(CHOICE_MARGIN, CHOICE_MARGIN, CHOICE_W, CHOICE_H);
        bool is_lit = m_is_chosen || underMouse();

        if (is_lit) {
            qreal reach = SELECTION_RING_OFFSET + SELECTION_RING_WIDTH / 2;
            qreal ring_radius = STLXTHEME_THUMBNAIL_RADIUS + reach;
            uint32_t ring = m_is_chosen ? STLXTHEME_TEXT : STLXTHEME_LINE_STRONG;
            painter.setPen(QPen(theme_color(ring), SELECTION_RING_WIDTH));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(picture.adjusted(-reach, -reach, reach, reach), ring_radius, ring_radius);
        }

        paint_rounded_image(painter, m_image, picture);

        QRectF name_line(0, picture.bottom() + CHOICE_NAME_GAP, width(), CAPTION_LINE_HEIGHT);
        painter.setFont(ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal));
        painter.setPen(theme_color(is_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_DIM));
        painter.drawText(name_line, Qt::AlignCenter, painter.fontMetrics().elidedText(m_name, Qt::ElideRight, width()));
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        bool is_click = event->button() == Qt::LeftButton && rect().contains(event->position().toPoint());
        if (is_click && on_choose) {
            on_choose(m_path);
        }
    }

private:
    QString m_path;
    QString m_name;
    QImage m_image;
    bool m_is_chosen = false;
};

/* The color the desktop shows when no wallpaper is set or it cannot be read, as a swatch and its hex */
class fallback_chip : public QWidget {
public:
    explicit fallback_chip(uint32_t color) : m_color(color), m_hex(hex_text(color)) {
        setFixedHeight(CAPTION_LINE_HEIGHT);
    }

    QSize sizeHint() const override {
        qreal label_width = QFontMetricsF(ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal)).horizontalAdvance(label());
        qreal hex_width = QFontMetricsF(mono_font(STLXTHEME_CAPTION_SIZE)).horizontalAdvance(m_hex);
        return QSize(qCeil(label_width + hex_width) + CHIP_DOT_SIZE + 2 * CHIP_GAP, CAPTION_LINE_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont label_font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal);
        qreal label_width = QFontMetricsF(label_font).horizontalAdvance(label());

        painter.setFont(label_font);
        painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
        painter.drawText(QRectF(0, 0, label_width, height()), Qt::AlignVCenter, label());

        QRectF dot(
            label_width + CHIP_GAP + 0.5,
            (height() - CHIP_DOT_SIZE) / 2.0 + 0.5,
            CHIP_DOT_SIZE - 1,
            CHIP_DOT_SIZE - 1
        );
        painter.setPen(QPen(theme_color(STLXTHEME_LINE_STRONG), 1));
        painter.setBrush(QColor::fromRgb(m_color));
        painter.drawEllipse(dot);

        painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
        painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
        painter.drawText(QRectF(dot.right() + 0.5 + CHIP_GAP, 0, width(), height()), Qt::AlignVCenter, m_hex);
    }

private:
    static QString label() { return QStringLiteral("Fallback"); }

    uint32_t m_color;
    QString m_hex;
};

/* The accent as a readout, since amber is the only accent: a ringed swatch, its name and its hex */
class accent_readout : public QWidget {
public:
    accent_readout() : m_hex(hex_text(STLXTHEME_ACCENT)) {
        setFixedHeight(STLXTHEME_CONTROL_HEIGHT);
    }

    QSize sizeHint() const override {
        qreal name_width = QFontMetricsF(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium)).horizontalAdvance(name());
        qreal hex_width = QFontMetricsF(mono_font(STLXTHEME_BODY_SIZE)).horizontalAdvance(m_hex);
        return QSize(qCeil(swatch_reach() * 2 + ACCENT_GAP + name_width + ACCENT_GAP + hex_width), height());
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPointF center(swatch_reach(), height() / 2.0);

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_color(STLXTHEME_ACCENT));
        painter.drawEllipse(center, ACCENT_DOT_SIZE / 2.0, ACCENT_DOT_SIZE / 2.0);

        qreal ring_radius = ACCENT_DOT_SIZE / 2.0 + SELECTION_RING_OFFSET + SELECTION_RING_WIDTH / 2;
        painter.setPen(QPen(theme_color(STLXTHEME_TEXT), SELECTION_RING_WIDTH));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, ring_radius, ring_radius);

        QFont name_font = ui_font(STLXTHEME_BODY_SIZE, QFont::Medium);
        qreal name_left = 2 * swatch_reach() + ACCENT_GAP;
        painter.setFont(name_font);
        painter.setPen(theme_color(STLXTHEME_TEXT));
        painter.drawText(QRectF(name_left, 0, width(), height()), Qt::AlignVCenter, name());

        qreal hex_left = name_left + QFontMetricsF(name_font).horizontalAdvance(name()) + ACCENT_GAP;
        painter.setFont(mono_font(STLXTHEME_BODY_SIZE));
        painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
        painter.drawText(QRectF(hex_left, 0, width(), height()), Qt::AlignVCenter, m_hex);
    }

private:
    static QString name() { return QLatin1String(ACCENT_NAME); }

    /* From the swatch's center to the outside of its selection ring */
    static qreal swatch_reach() { return ACCENT_DOT_SIZE / 2.0 + SELECTION_RING_OFFSET + SELECTION_RING_WIDTH; }

    QString m_hex;
};

/* The widgets a wallpaper change refreshes, valid while the Appearance page shows */
struct wallpaper_widgets {
    wallpaper_preview* preview = nullptr;
    QLineEdit* field = nullptr;
    QLabel* details = nullptr;
    QList<wallpaper_choice*> choices;
};

} // namespace

static wallpaper_widgets g_wallpaper;

/* `image` must be the size of `box`. Fills the rounded box with `image` and draws a faint cream ring inside its edge */
static void paint_rounded_image(QPainter& painter, const QImage& image, const QRectF& box) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    QBrush picture(image);
    picture.setTransform(QTransform::fromTranslate(box.left(), box.top()));

    painter.setPen(Qt::NoPen);
    painter.setBrush(picture);
    painter.drawRoundedRect(box, STLXTHEME_THUMBNAIL_RADIUS, STLXTHEME_THUMBNAIL_RADIUS);

    qreal inset = 0.5;
    qreal ring_radius = STLXTHEME_THUMBNAIL_RADIUS - inset;
    painter.setPen(QPen(theme_color(STLXTHEME_THUMBNAIL_RING), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(box.adjusted(inset, inset, -inset, -inset), ring_radius, ring_radius);
    painter.restore();
}

static QString hex_text(uint32_t color) {
    return QStringLiteral("#%1").arg(color & 0xFFFFFFu, 6, 16, QLatin1Char('0')).toUpper();
}

static QImage decoded_wallpaper(const QString& path) {
    auto cached = g_decoded_wallpapers.constFind(path);
    if (cached != g_decoded_wallpapers.constEnd()) {
        return cached.value();
    }

    QImageReader reader(path);
    QSize full_size = reader.size();
    if (full_size.isValid()) {
        reader.setScaledSize(full_size.scaled(DECODE_W, DECODE_H, Qt::KeepAspectRatioByExpanding));
    }

    QImage image = reader.read();
    if (!image.isNull()) {
        g_decoded_wallpapers.insert(path, image);
    }

    return image;
}

static QImage cover_image(const QString& path, const QSize& size) {
    QImage decoded = decoded_wallpaper(path);
    if (decoded.isNull()) {
        QImage flat(size, QImage::Format_RGB32);
        flat.fill(QColor::fromRgb(g_conf.bg_color));
        return flat;
    }

    QImage covering = decoded.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPoint corner((covering.width() - size.width()) / 2, (covering.height() - size.height()) / 2);

    return covering.copy(QRect(corner, size));
}

QImage wallpaper_picture(const QSize& size) {
    return cover_image(QString::fromUtf8(g_conf.wallpaper), size);
}

static QImage compose_preview(const QString& path) {
    QImage preview = cover_image(path, QSize(PREVIEW_W, PREVIEW_H));
    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(QRect(0, 0, PREVIEW_W, PREVIEW_BAR_H), theme_color(STLXTHEME_PREVIEW_CHROME));

    QRectF dock(
        (PREVIEW_W - PREVIEW_DOCK_W) / 2.0,
        PREVIEW_H - PREVIEW_DOCK_BOTTOM - PREVIEW_DOCK_H,
        PREVIEW_DOCK_W,
        PREVIEW_DOCK_H
    );
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme_color(STLXTHEME_PREVIEW_CHROME));
    painter.drawRoundedRect(dock, PREVIEW_DOCK_RADIUS, PREVIEW_DOCK_RADIUS);
    painter.end();

    return preview;
}

/* Size, format and file size joined by middle dots, or why the desktop shows the fallback color */
static QString image_details(const QString& path) {
    if (path.isEmpty()) {
        return QStringLiteral("No image, the desktop shows the fallback color");
    }

    QImageReader reader(path);
    QSize size = reader.size();
    if (!size.isValid()) {
        return QStringLiteral("Cannot be read, the desktop shows the fallback color");
    }

    QString format = QString::fromLatin1(reader.format()).toUpper();
    qint64 kilobytes = (QFileInfo(path).size() + BYTES_PER_KB / 2) / BYTES_PER_KB;

    return QStringLiteral("%1 %2 %3 %4 %5 %4 %6 KB")
        .arg(size.width())
        .arg(TIMES_SIGN)
        .arg(size.height())
        .arg(MIDDLE_DOT)
        .arg(format)
        .arg(kilobytes);
}

static void show_wallpaper(const QString& path) {
    g_wallpaper.preview->set_image(compose_preview(path));
    g_wallpaper.details->setText(image_details(path));
    for (wallpaper_choice* choice : g_wallpaper.choices) {
        choice->set_chosen(choice->path() == path);
    }
}

static void choose_wallpaper(const QString& path) {
    copy_string(g_conf.wallpaper, sizeof(g_conf.wallpaper), path);
    g_wallpaper.field->setText(path);
    g_wallpaper.field->setCursorPosition(0);
    show_wallpaper(path);
    show_pending_changes();
}

static QWidget* make_wallpaper_details() {
    auto* details = new QWidget;
    auto* layout = new QVBoxLayout(details);
    layout->setContentsMargins(0, DETAILS_PADDING_Y, 0, DETAILS_PADDING_Y);
    layout->setSpacing(DETAILS_SPACING);
    layout->addWidget(new setting_label(QLatin1String(IMAGE_LABEL), QLatin1String(WALLPAPER_KEY)));

    g_wallpaper.field = new QLineEdit(QString::fromUtf8(g_conf.wallpaper));
    g_wallpaper.field->setFont(mono_font(STLXTHEME_BODY_SIZE));
    g_wallpaper.field->setCursorPosition(0);
    layout->addWidget(g_wallpaper.field);

    QObject::connect(g_wallpaper.field, &QLineEdit::textEdited, [](const QString& path) {
        copy_string(g_conf.wallpaper, sizeof(g_conf.wallpaper), path);
        show_wallpaper(QString::fromUtf8(g_conf.wallpaper));
        show_pending_changes();
    });

    auto* caption = new QHBoxLayout;
    g_wallpaper.details = make_text(QString(), ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal), STLXTHEME_TEXT_DIM);
    g_wallpaper.details->setFixedHeight(CAPTION_LINE_HEIGHT);
    caption->addWidget(g_wallpaper.details, 1);
    caption->addWidget(new fallback_chip(g_conf.bg_color));
    layout->addLayout(caption);

    return details;
}

/* The summary is the image's row, named for search as a setting row is */
static QWidget* make_wallpaper_summary() {
    auto* summary = new QWidget;
    summary->setObjectName(QLatin1String(IMAGE_LABEL));
    auto* layout = new QHBoxLayout(summary);
    layout->setContentsMargins(
        WALLPAPER_CARD_PADDING,
        WALLPAPER_CARD_PADDING,
        DETAILS_PADDING_RIGHT,
        WALLPAPER_CARD_PADDING
    );
    layout->setSpacing(DETAILS_GAP);

    g_wallpaper.preview = new wallpaper_preview;
    layout->addWidget(g_wallpaper.preview);
    layout->addWidget(make_wallpaper_details(), 1);

    return summary;
}

static QWidget* make_wallpaper_choices() {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(CHOICES_PADDING_X, CHOICES_PADDING_Y, CHOICES_PADDING_X, CHOICES_PADDING_Y);
    layout->setSpacing(CHOICE_SPACING);

    /* The formats stlxdm reads a wallpaper in, so every choice can show on the desktop */
    const QStringList patterns { QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
                                 QStringLiteral("*.bmp") };
    QDir directory(QString::fromLatin1(WALLPAPER_DIR));
    for (const QFileInfo& file : directory.entryInfoList(patterns, QDir::Files, QDir::Name)) {
        auto* choice = new wallpaper_choice(file.filePath(), cover_image(file.filePath(), QSize(CHOICE_W, CHOICE_H)));
        choice->on_choose = choose_wallpaper;
        g_wallpaper.choices.append(choice);
        layout->addWidget(choice);
    }

    layout->addStretch(1);

    return row;
}

/* A level's square fills more the less wallpaper its glass shows, full at Solid and empty at Clear */
static void paint_glass_mark(QPainter& painter, const QRectF& box, int level, const QColor& color) {
    QColor fill = color;
    fill.setAlphaF(color.alphaF() * GLASS_MARK_FILLS[level]);

    painter.setPen(QPen(color, 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), SEGMENT_MARK_RADIUS - 0.5, SEGMENT_MARK_RADIUS - 0.5);
}

static void paint_tint_mark(QPainter& painter, const QRectF& box, int tint, const QColor&) {
    painter.setPen(QPen(theme_color(STLXTHEME_TINT_SWATCH_RING), 1));
    painter.setBrush(theme_color(TINT_MARK_COLORS[tint]));
    painter.drawEllipse(box.adjusted(0.5, 0.5, -0.5, -0.5));
}

static QString glass_key() {
    QString level = QString::fromLatin1(stlxconf_glass_level_name(g_conf.glass_level));
    return QStringLiteral("%1 = %2").arg(QLatin1String(GLASS_KEY), level);
}

/* The position of the configured tint in `TINT_VALUES`, which must list every tint */
static int chosen_tint() {
    const auto* chosen = std::find(std::begin(TINT_VALUES), std::end(TINT_VALUES), g_conf.glass_tint);
    return static_cast<int>(chosen - std::begin(TINT_VALUES));
}

static QString tint_key() {
    QString tint = QString::fromLatin1(stlxconf_glass_tint_name(g_conf.glass_tint));
    return QStringLiteral("%1 = %2").arg(QLatin1String(TINT_KEY), tint);
}

static QStringList names_of(const char* const* names, size_t count) {
    QStringList list;
    for (size_t i = 0; i < count; i++) {
        list.append(QString::fromLatin1(names[i]));
    }

    return list;
}

static void add_glass_row(card* style) {
    auto* row = new setting_row(QLatin1String(GLASS_LABEL), glass_key());
    auto* levels = new segmented(
        names_of(GLASS_LEVEL_NAMES, std::size(GLASS_LEVEL_NAMES)),
        g_conf.glass_level,
        paint_glass_mark
    );
    levels->on_choose = [row](int level) {
        g_conf.glass_level = static_cast<stlxconf_glass_level_t>(level);
        row->set_key(glass_key());
        show_pending_changes();
    };

    row->add_control(levels);
    style->add_row(row);
}

static void add_tint_row(card* style) {
    auto* row = new setting_row(QLatin1String(TINT_LABEL), tint_key());
    auto* tints = new segmented(names_of(TINT_NAMES, std::size(TINT_NAMES)), chosen_tint(), paint_tint_mark);
    tints->on_choose = [row](int tint) {
        g_conf.glass_tint = TINT_VALUES[tint];
        row->set_key(tint_key());
        show_pending_changes();
    };

    row->add_control(tints);
    style->add_row(row);
}

void build_appearance_page(QWidget* page) {
    g_wallpaper = wallpaper_widgets();
    card* wallpaper = add_section(page, QLatin1String(WALLPAPER_CARD), QStringLiteral("[desktop]"));
    wallpaper->add_row(make_wallpaper_summary());

    QWidget* choices = make_wallpaper_choices();
    if (g_wallpaper.choices.isEmpty()) {
        delete choices;
    } else {
        wallpaper->add_row(choices);
    }

    show_wallpaper(QString::fromUtf8(g_conf.wallpaper));

    card* style = add_section(page, QLatin1String(STYLE_CARD), QStringLiteral("[theme]"));
    add_glass_row(style);
    add_tint_row(style);

    auto* accent = new setting_row(QLatin1String(ACCENT_LABEL), QString());
    accent->add_control(new accent_readout);
    style->add_row(accent);
}

void add_appearance_entries(QList<search_entry>& entries) {
    QString wallpaper = QLatin1String(WALLPAPER_CARD);
    QString style = QLatin1String(STYLE_CARD);
    QString image = QFileInfo(QString::fromUtf8(g_conf.wallpaper)).fileName();
    QString level = QLatin1String(GLASS_LEVEL_NAMES[g_conf.glass_level]);
    QString tint = QLatin1String(TINT_NAMES[chosen_tint()]);

    /* Each of these rows takes its label as its object name */
    QString image_label = QLatin1String(IMAGE_LABEL);
    QString glass_label = QLatin1String(GLASS_LABEL);
    QString tint_label = QLatin1String(TINT_LABEL);
    QString accent_label = QLatin1String(ACCENT_LABEL);

    entries.append({ APPEARANCE_PAGE, wallpaper, image_label, QLatin1String(WALLPAPER_KEY), image, image_label });
    entries.append({ APPEARANCE_PAGE, style, glass_label, QLatin1String(GLASS_KEY), level, glass_label });
    entries.append({ APPEARANCE_PAGE, style, tint_label, QLatin1String(TINT_KEY), tint, tint_label });
    entries.append({ APPEARANCE_PAGE, style, accent_label, QString(), QLatin1String(ACCENT_NAME), accent_label });
}
