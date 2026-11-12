#include "drag.h"
#include "clipboard.h"
#include "theme.h"
#include "window.h"

#include <QDrag>
#include <QEventLoop>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QUrl>

#include <stlxtheme/theme.h>

#include <algorithm>

// The default text picture: up to three lines of mono text in a pocket, elided past a width
static const int TEXT_LINES_MAX = 3;
static const int TEXT_LINE_HEIGHT = 18;
static const int TEXT_WIDTH_MAX = 360;
static const int POCKET_PAD_X = 10;
static const int POCKET_PAD_Y = 6;
static const float TEXT_EM_PX = 12.5f;
static const qreal POCKET_OPACITY = 0.94;

// The default file picture: the first file's name on a plate
static const int PLATE_HEIGHT = 40;
static const int PLATE_PAD_X = 14;
static const float NAME_EM_PX = 13.5f;

// A tab in dragged text shows as this many spaces
static const int TAB_SPACES = 4;

// A default picture lies this far right of and below the pointer's tip, clear of the arrow
static const QPoint DEFAULT_PICTURE_OFFSET(12, 16);

uint32_t to_stlxwin_actions(Qt::DropActions actions) {
    uint32_t desktop = 0;
    if (actions & Qt::CopyAction) {
        desktop |= STLXWIN_DND_COPY;
    }

    if (actions & (Qt::MoveAction | Qt::TargetMoveAction)) {
        desktop |= STLXWIN_DND_MOVE;
    }

    if (actions & Qt::LinkAction) {
        desktop |= STLXWIN_DND_LINK;
    }

    return desktop;
}

Qt::DropActions to_qt_actions(uint32_t actions) {
    Qt::DropActions qt = Qt::IgnoreAction;
    if (actions & STLXWIN_DND_COPY) {
        qt |= Qt::CopyAction;
    }

    if (actions & STLXWIN_DND_MOVE) {
        qt |= Qt::MoveAction;
    }

    if (actions & STLXWIN_DND_LINK) {
        qt |= Qt::LinkAction;
    }

    return qt;
}

static Qt::DropAction to_qt_action(uint32_t action) {
    switch (action) {
        case STLXWIN_DND_COPY:
            return Qt::CopyAction;
        case STLXWIN_DND_MOVE:
            return Qt::MoveAction;
        case STLXWIN_DND_LINK:
            return Qt::LinkAction;
        default:
            return Qt::IgnoreAction;
    }
}

static QImage clear_picture(const QSize& size) {
    QImage picture(size, QImage::Format_ARGB32_Premultiplied);
    picture.fill(Qt::transparent);

    return picture;
}

static QImage text_picture(QString text) {
    QFont font = desktop_font(STLXTHEME_MONO_FONT_FAMILY, TEXT_EM_PX, QFont::Normal);
    QFontMetrics metrics(font);
    QStringList lines = text.replace(QLatin1Char('\t'), QString(TAB_SPACES, QLatin1Char(' '))).split(QLatin1Char('\n'));
    lines = lines.mid(0, TEXT_LINES_MAX);

    int text_w = 0;
    for (QString& line : lines) {
        line = metrics.elidedText(line, Qt::ElideRight, TEXT_WIDTH_MAX);
        text_w = std::max(text_w, metrics.horizontalAdvance(line));
    }

    QImage picture = clear_picture(QSize(text_w + 2 * POCKET_PAD_X, lines.size() * TEXT_LINE_HEIGHT + 2 * POCKET_PAD_Y));
    QPainter painter(&picture);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setOpacity(POCKET_OPACITY);
    painter.setPen(QColor::fromRgba(STLXTHEME_LINE));
    painter.setBrush(QColor::fromRgba(STLXTHEME_OPAQUE_POCKET));
    painter.drawRoundedRect(QRectF(picture.rect()).adjusted(0.5, 0.5, -0.5, -0.5), STLXTHEME_CONTROL_RADIUS,
                            STLXTHEME_CONTROL_RADIUS);

    painter.setOpacity(1);
    painter.setFont(font);
    painter.setPen(QColor::fromRgba(STLXTHEME_TERMINAL_TEXT));
    for (int i = 0; i < lines.size(); i++) {
        QRect line(POCKET_PAD_X, POCKET_PAD_Y + i * TEXT_LINE_HEIGHT, text_w, TEXT_LINE_HEIGHT);
        painter.drawText(line, Qt::AlignLeft | Qt::AlignVCenter, lines[i]);
    }

    return picture;
}

static QImage file_picture(const QUrl& url) {
    QFont font = desktop_font(STLXTHEME_UI_FONT_FAMILY, NAME_EM_PX, QFont::Medium);
    QFontMetrics metrics(font);
    QString name = url.isLocalFile() ? QFileInfo(url.toLocalFile()).fileName() : url.toString();
    name = metrics.elidedText(name, Qt::ElideMiddle, TEXT_WIDTH_MAX);

    QImage picture = clear_picture(QSize(metrics.horizontalAdvance(name) + 2 * PLATE_PAD_X, PLATE_HEIGHT));
    QPainter painter(&picture);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor::fromRgba(STLXTHEME_RIM));
    painter.setBrush(QColor::fromRgba(STLXTHEME_MENU_BG));
    painter.drawRoundedRect(QRectF(picture.rect()).adjusted(0.5, 0.5, -0.5, -0.5), STLXTHEME_MENU_RADIUS,
                            STLXTHEME_MENU_RADIUS);

    painter.setFont(font);
    painter.setPen(QColor::fromRgba(STLXTHEME_TEXT));
    painter.drawText(picture.rect().adjusted(PLATE_PAD_X, 0, -PLATE_PAD_X, 0), Qt::AlignLeft | Qt::AlignVCenter, name);

    return picture;
}

// A default picture with a clear margin before it, so stlxdm holds it by the margin's corner
static QImage offset_picture(const QImage& picture) {
    if (picture.isNull()) {
        return picture;
    }

    QImage offset = clear_picture(picture.size() + QSize(DEFAULT_PICTURE_OFFSET.x(), DEFAULT_PICTURE_OFFSET.y()));
    QPainter painter(&offset);
    painter.drawImage(DEFAULT_PICTURE_OFFSET, picture);

    return offset;
}

// The drag's picture in premultiplied ARGB within stlxdm's size, with `hot` moved to match: its own, else its first
// file's name on a plate or its text in a pocket, below and right of the pointer
static QImage drag_picture(QDrag* drag, QPoint* hot) {
    QImage picture = drag->pixmap().toImage();
    if (picture.isNull()) {
        const QMimeData* data = drag->mimeData();
        if (data->hasUrls() && !data->urls().isEmpty()) {
            picture = offset_picture(file_picture(data->urls().first()));
        } else if (data->hasText() && !data->text().isEmpty()) {
            picture = offset_picture(text_picture(data->text()));
        }

        *hot = QPoint(0, 0);
    }

    if (picture.isNull()) {
        return picture;
    }

    int largest = STLXWIN_DRAG_PICTURE_MAX;
    if (picture.width() > largest || picture.height() > largest) {
        QSize before = picture.size();
        picture = picture.scaled(largest, largest, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        *hot = QPoint(hot->x() * picture.width() / before.width(), hot->y() * picture.height() / before.height());
    }

    return picture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QStelluxDrag::QStelluxDrag(stlxwin_conn* connection)
    : m_connection(connection) {
}

void QStelluxDrag::note_press(QWindow* window, uint32_t serial) {
    m_press_window = window;
    m_press_serial = serial;
}

void QStelluxDrag::handle_state(const stlxwin_event& event) {
    bool is_running = m_loop && event.drag_state.drag == m_drag;
    if (!is_running || event.drag_state.state == STLXWIN_DRAG_STARTED) {
        return;
    }

    bool is_finished = event.drag_state.state == STLXWIN_DRAG_FINISHED;
    m_result = is_finished ? to_qt_action(event.drag_state.action) : Qt::IgnoreAction;
    m_loop->quit();
}

// The drag starts from the app's last press, runs in a loop until stlxdm reports how it ended, and returns the action
// the target took, which Scintilla and Qt's item views act on
Qt::DropAction QStelluxDrag::drag(QDrag* drag) {
    QWindow* window = m_press_window.data();
    auto* source = window ? static_cast<QStelluxWindow*>(window->handle()) : nullptr;
    if (!source || !source->handle() || m_press_serial == 0 || m_loop) {
        return Qt::IgnoreAction;
    }

    // A drag of formats only the app reads offers the desktop nothing, so only the app's own windows take it
    const QMimeData* data = drag->mimeData();
    QList<representation> reps = representations(data);
    uint32_t actions = to_stlxwin_actions(drag->supportedActions());
    if (actions == 0) {
        return Qt::IgnoreAction;
    }

    QByteArray names[STLXWIN_OFFER_TYPES_MAX];
    stlxwin_offer_rep wire[STLXWIN_OFFER_TYPES_MAX];
    uint32_t count = 0;
    for (const representation& rep : reps) {
        names[count] = rep.first.toUtf8();
        wire[count] = { names[count].constData(), rep.second.constData(), static_cast<size_t>(rep.second.size()) };
        count++;
    }

    QPoint hot = drag->hotSpot();
    QImage picture = drag_picture(drag, &hot);
    stlxwin_drag_picture shown = { reinterpret_cast<const uint32_t*>(picture.constBits()),
                                   static_cast<uint32_t>(picture.width()), static_cast<uint32_t>(picture.height()),
                                   static_cast<uint32_t>(picture.bytesPerLine()), hot.x(), hot.y() };

    uint32_t items = data->hasUrls() ? static_cast<uint32_t>(std::max<qsizetype>(1, data->urls().size())) : 1;
    const stlxwin_drag_picture* shown_picture = picture.isNull() ? nullptr : &shown;
    if (stlxwin_drag_start(source->handle(), m_press_serial, wire, count, shown_picture, actions, items) != 0) {
        return Qt::IgnoreAction;
    }

    QEventLoop loop;
    m_loop = &loop;
    m_drag = m_press_serial;
    m_result = Qt::IgnoreAction;
    loop.exec();

    m_loop = nullptr;
    m_drag = 0;

    // stlxdm took the press's release, which the window never hears
    window = m_press_window.data();
    if (window && window->handle()) {
        static_cast<QStelluxWindow*>(window->handle())->forget_buttons();
    }

    return m_result;
}

void QStelluxDrag::cancelDrag() {
    if (m_loop) {
        stlxwin_drag_cancel(m_connection, m_drag);
    }
}

// Alt asks for no action, as stlxdm keeps it for one that asks at the drop
Qt::DropAction QStelluxDrag::defaultAction(Qt::DropActions possible_actions, Qt::KeyboardModifiers modifiers) const {
    return QPlatformDrag::defaultAction(possible_actions, modifiers & ~Qt::AltModifier);
}
