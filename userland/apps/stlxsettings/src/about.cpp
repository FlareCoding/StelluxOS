#include "kit.hpp"
#include "settings.hpp"

#include <stlxtheme/theme.h>
#include <stlxwin/stlxwin.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include <sys/utsname.h>

constexpr const char* CPU_STATS_PATH = "/dev/sysinfo/cpu";
constexpr const char* MEMORY_STATS_PATH = "/dev/sysinfo/mem";
constexpr const char* UPTIME_PATH = "/dev/sysinfo/uptime";
constexpr const char* FONT_DIR = "/etc/res/fonts";
constexpr const char* LICENSE_PREFIX = "LICENSE-";

constexpr int IDENTITY_GAP_ABOVE = 16;
constexpr int IDENTITY_PADDING_Y = 16;
constexpr int IDENTITY_PADDING_LEFT = 20;
constexpr int IDENTITY_GAP = 18;
constexpr int IDENTITY_LOGO_SIZE = 44;
constexpr float NAME_SIZE = 22.0f;
constexpr int NAME_LINE_HEIGHT = 28;
constexpr qreal NAME_TRACKING_EM = -0.01;

constexpr int FACTS_PADDING_Y = 4;
constexpr int FACT_ROW_H = 32;
constexpr int FACT_KEY_W = 120;
constexpr int FACT_PADDING_X = 14;
constexpr int FACT_DETAIL_GAP = 10;

constexpr int LICENSES_PADDING = 4;
constexpr int LICENSE_ROW_H = 34;
constexpr int LICENSE_PADDING_X = 10;
constexpr int LICENSE_GLYPH_GAP = 10;
constexpr int LICENSE_COLUMNS = 2;

constexpr int REFRESH_MS = 1000;

constexpr uint64_t NS_PER_SECOND = 1000000000;
constexpr uint64_t SECONDS_PER_MINUTE = 60;
constexpr uint64_t SECONDS_PER_HOUR = 3600;
constexpr uint64_t SECONDS_PER_DAY = 86400;
constexpr uint64_t BYTES_PER_MIB = 1024 * 1024;
constexpr qreal BYTES_PER_GIB = 1024.0 * 1024 * 1024;
constexpr int SIZE_PRECISION = 3;

namespace {

/* A machine fact: its caps key, empty when it continues the fact above, its value and a Plex Mono detail */
struct fact {
    QString key;
    QString value;
    QString detail;
};

/* A license shipped in the image, named for what it covers */
struct license {
    QString name;
    QString id;
};

/* The system's logo, its name and release, and the kernel build under them */
class identity_view : public QWidget {
public:
    identity_view(const QString& name, const QString& build) : m_name(name), m_build(build) {
        setFixedHeight(2 * IDENTITY_PADDING_Y + IDENTITY_LOGO_SIZE);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QRectF logo(IDENTITY_PADDING_LEFT, IDENTITY_PADDING_Y, IDENTITY_LOGO_SIZE, IDENTITY_LOGO_SIZE);
        paint_glyph(painter, glyph::LOGO, logo, theme_color(STLXTHEME_TEXT));

        qreal text_left = logo.right() + IDENTITY_GAP;
        qreal text_width = width() - text_left;
        qreal name_top = (height() - NAME_LINE_HEIGHT - CAPTION_LINE_HEIGHT) / 2.0;
        QFont name_font = ui_font(NAME_SIZE, QFont::DemiBold);
        name_font.setLetterSpacing(QFont::AbsoluteSpacing, NAME_TRACKING_EM * NAME_SIZE);

        painter.setFont(name_font);
        painter.setPen(theme_color(STLXTHEME_TEXT));
        painter.drawText(QRectF(text_left, name_top, text_width, NAME_LINE_HEIGHT), Qt::AlignVCenter, m_name);

        QRectF build_line(text_left, name_top + NAME_LINE_HEIGHT, text_width, CAPTION_LINE_HEIGHT);
        painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
        painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
        painter.drawText(build_line, Qt::AlignVCenter, m_build);
    }

private:
    QString m_name;
    QString m_build;
};

/* Facts one under another, the key in a fixed column, a hairline between neighbors */
class fact_list : public QWidget {
public:
    void set_facts(const QList<fact>& facts) {
        m_facts = facts;
        setFixedHeight(2 * FACTS_PADDING_Y + static_cast<int>(facts.size()) * FACT_ROW_H);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QFont value_font = ui_font(STLXTHEME_BODY_SIZE, QFont::Normal);
        QFont detail_font = mono_font(STLXTHEME_CAPTION_SIZE);
        qreal value_left = FACT_KEY_W + FACT_PADDING_X;

        for (int i = 0; i < m_facts.size(); i++) {
            const fact& shown = m_facts[i];
            int top = FACTS_PADDING_Y + i * FACT_ROW_H;
            if (i > 0) {
                painter.fillRect(QRect(0, top, width(), 1), theme_color(STLXTHEME_LINE_SUBTLE));
            }

            QRectF line(0, top + 1, width(), FACT_ROW_H - 1);
            painter.setFont(caps_font());
            painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
            painter.drawText(line.adjusted(FACT_PADDING_X, 0, 0, 0), Qt::AlignVCenter, shown.key.toUpper());

            painter.setFont(value_font);
            painter.setPen(theme_color(STLXTHEME_TEXT));
            painter.drawText(line.adjusted(value_left, 0, 0, 0), Qt::AlignVCenter, shown.value);

            qreal detail_left = value_left + QFontMetricsF(value_font).horizontalAdvance(shown.value) + FACT_DETAIL_GAP;
            painter.setFont(detail_font);
            painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
            painter.drawText(line.adjusted(detail_left, 0, 0, 0), Qt::AlignVCenter, shown.detail);
        }
    }

private:
    QList<fact> m_facts;
};

/* Licenses in two columns: a document glyph, what the license covers and its SPDX identifier at the right */
class license_list : public QWidget {
public:
    explicit license_list(const QList<license>& licenses) : m_licenses(licenses) {
        int rows = (static_cast<int>(licenses.size()) + LICENSE_COLUMNS - 1) / LICENSE_COLUMNS;
        setFixedHeight(2 * LICENSES_PADDING + rows * LICENSE_ROW_H);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        qreal column_width = (width() - 2.0 * LICENSES_PADDING) / LICENSE_COLUMNS;

        for (int i = 0; i < m_licenses.size(); i++) {
            qreal left = LICENSES_PADDING + (i % LICENSE_COLUMNS) * column_width;
            qreal top = LICENSES_PADDING + (i / LICENSE_COLUMNS) * LICENSE_ROW_H;
            QRectF cell = QRectF(left, top, column_width, LICENSE_ROW_H).adjusted(LICENSE_PADDING_X, 0,
                                                                                  -LICENSE_PADDING_X, 0);
            QRectF glyph_box(cell.left(), cell.center().y() - STLXTHEME_GLYPH_SIZE / 2.0, STLXTHEME_GLYPH_SIZE,
                             STLXTHEME_GLYPH_SIZE);
            paint_glyph(painter, glyph::DOCUMENT, glyph_box, theme_color(STLXTHEME_TEXT_DIM));

            painter.setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium));
            painter.setPen(theme_color(STLXTHEME_TEXT));
            painter.drawText(cell.adjusted(STLXTHEME_GLYPH_SIZE + LICENSE_GLYPH_GAP, 0, 0, 0), Qt::AlignVCenter,
                             m_licenses[i].name);

            painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
            painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
            painter.drawText(cell, Qt::AlignVCenter | Qt::AlignRight, m_licenses[i].id);
        }
    }

private:
    QList<license> m_licenses;
};

} // namespace

static QByteArray read_text(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }

    return file.readAll();
}

/* The number on the line `label` starts, as /dev/sysinfo/mem writes its lines, or 0 without such a line */
static uint64_t labeled_number(const QByteArray& text, const QByteArray& label) {
    for (const QByteArray& line : text.split('\n')) {
        QList<QByteArray> words = line.simplified().split(' ');
        if (words.size() == 2 && words[0] == label) {
            return words[1].toULongLong();
        }
    }

    return 0;
}

static QString memory_size_text(uint64_t bytes) {
    if (bytes >= BYTES_PER_GIB) {
        return QStringLiteral("%1 GiB").arg(bytes / BYTES_PER_GIB, 0, 'g', SIZE_PRECISION);
    }

    return QStringLiteral("%1 MiB").arg(bytes / BYTES_PER_MIB);
}

static QString duration_text(uint64_t seconds) {
    uint64_t days = seconds / SECONDS_PER_DAY;
    uint64_t hours = seconds % SECONDS_PER_DAY / SECONDS_PER_HOUR;
    uint64_t minutes = seconds % SECONDS_PER_HOUR / SECONDS_PER_MINUTE;
    if (days > 0) {
        return QStringLiteral("%1 d %2 h").arg(days).arg(hours);
    }

    if (hours > 0) {
        return QStringLiteral("%1 h %2 min").arg(hours).arg(minutes);
    }

    if (minutes > 0) {
        return QStringLiteral("%1 min").arg(minutes);
    }

    return QStringLiteral("%1 s").arg(seconds);
}

/* /dev/sysinfo/cpu writes one line for each CPU */
static fact cpu_fact() {
    int count = 0;
    for (const QByteArray& line : read_text(QLatin1String(CPU_STATS_PATH)).split('\n')) {
        count += line.startsWith("cpu") ? 1 : 0;
    }

    return { QStringLiteral("CPU"), count == 1 ? QStringLiteral("1 CPU") : QStringLiteral("%1 CPUs").arg(count),
             QString() };
}

static fact memory_fact() {
    QByteArray text = read_text(QLatin1String(MEMORY_STATS_PATH));
    uint64_t page_size = labeled_number(text, "page_size");
    uint64_t total = labeled_number(text, "total_pages") * page_size;
    uint64_t used = labeled_number(text, "used_pages") * page_size;

    return { QStringLiteral("Memory"), memory_size_text(total),
             QStringLiteral("%1 in use").arg(memory_size_text(used)) };
}

/* The boot time reads in UTC, as the bar's clock does */
static fact uptime_fact() {
    uint64_t seconds = read_text(QLatin1String(UPTIME_PATH)).trimmed().toULongLong() / NS_PER_SECOND;
    QDateTime boot = QDateTime::currentDateTimeUtc().addSecs(-static_cast<qint64>(seconds));
    QString since = QStringLiteral("since %1").arg(boot.toString(QStringLiteral("HH:mm")));

    return { QStringLiteral("Uptime"), duration_text(seconds), since };
}

/* Each display's name and mode, or a fact saying none are listed */
static QList<fact> display_facts() {
    stlxwin_output outputs[STLXWIN_OUTPUTS_MAX] = {};
    long listed = 0;
    uint32_t generation = 0;
    stlxwin_conn* conn = stlxwin_connect("stlxsettings");
    if (conn) {
        listed = stlxwin_outputs_get(conn, &generation, outputs, STLXWIN_OUTPUTS_MAX);
        stlxwin_disconnect(conn);
    }

    QString key = QStringLiteral("Displays");
    uint32_t count = static_cast<uint32_t>(qBound(0L, listed, static_cast<long>(STLXWIN_OUTPUTS_MAX)));
    if (count == 0) {
        return { { key, QStringLiteral("None listed"), QString() } };
    }

    QList<fact> facts;
    for (uint32_t i = 0; i < count; i++) {
        facts.append({ i == 0 ? key : QString(), display_name(outputs[i], i), mode_text(outputs[i]) });
    }

    return facts;
}

static QList<fact> machine_facts(const QList<fact>& displays) {
    QList<fact> facts { cpu_fact(), memory_fact() };
    facts.append(displays);
    facts.append(uptime_fact());

    return facts;
}

/* IBMPlexSans of LICENSE-IBMPlexSans.txt reads IBM Plex Sans */
static QString spaced_words(QString name) {
    name.replace(QRegularExpression(QStringLiteral("([a-z])([A-Z])")), QStringLiteral("\\1 \\2"));
    name.replace(QRegularExpression(QStringLiteral("([A-Z])([A-Z][a-z])")), QStringLiteral("\\1 \\2"));
    return name;
}

/* The SPDX identifier of the license a text grants, empty when it is not one Settings knows */
static QString license_id(const QString& text) {
    QRegularExpression open_font(QStringLiteral("Open Font License, Version (\\d+\\.\\d+)"));
    QRegularExpressionMatch match = open_font.match(text);
    return match.hasMatch() ? QStringLiteral("OFL-%1").arg(match.captured(1)) : QString();
}

static QList<license> font_licenses() {
    QList<license> licenses;
    QDir directory(QString::fromLatin1(FONT_DIR));
    QString pattern = QStringLiteral("%1*").arg(QLatin1String(LICENSE_PREFIX));
    for (const QFileInfo& file : directory.entryInfoList({ pattern }, QDir::Files, QDir::Name)) {
        QString covered = file.completeBaseName().mid(static_cast<qsizetype>(strlen(LICENSE_PREFIX)));
        QString text = QString::fromUtf8(read_text(file.filePath()));
        licenses.append({ spaced_words(covered), license_id(text) });
    }

    return licenses;
}

void build_about_page(QWidget* page) {
    utsname system = {};
    uname(&system);
    QString name = system_title(system);
    QString build = QStringLiteral("kernel %1 %2 %3")
                        .arg(QString::fromUtf8(system.release))
                        .arg(MIDDLE_DOT)
                        .arg(QString::fromUtf8(system.version));

    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    auto* identity = new card;
    identity->add_row(new identity_view(name, build));
    layout->addSpacing(IDENTITY_GAP_ABOVE);
    layout->addWidget(identity);

    /* Memory in use and uptime change while the page shows, the displays only when it rebuilds */
    QList<fact> displays = display_facts();
    auto* facts = new fact_list;
    facts->set_facts(machine_facts(displays));
    auto* refresh = new QTimer(facts);
    QObject::connect(refresh, &QTimer::timeout, [facts, displays]() { facts->set_facts(machine_facts(displays)); });
    refresh->start(REFRESH_MS);

    add_section(page, QStringLiteral("Hardware"), QStringLiteral("live"))->add_row(facts);

    QList<license> licenses = font_licenses();
    if (!licenses.isEmpty()) {
        card* section = add_section(page, QStringLiteral("Licenses"), QString::fromLatin1(FONT_DIR));
        section->add_row(new license_list(licenses));
    }
}
