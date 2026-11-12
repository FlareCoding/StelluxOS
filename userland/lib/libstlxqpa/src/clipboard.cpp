#include "clipboard.h"
#include "integration.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QImage>
#include <QList>
#include <QMetaObject>
#include <QPair>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>
#include <cstdlib>

// Qt names text without its charset and asks for an image as a QImage under a format of its own
static const QString QT_TEXT = QStringLiteral("text/plain");
static const QString QT_IMAGE = QStringLiteral("application/x-qt-image");
static const QString PNG = QStringLiteral("image/png");
static const QString URI_LIST = QStringLiteral("text/uri-list");
static const QString HTML = QStringLiteral("text/html");

// The hint password managers set on a copy, which keeps it out of the clipboard's history
static const QString PASSWORD_HINT = QStringLiteral("x-kde-passwordManagerHint");

// The desktop type a Qt format is read from
static QString desktop_type(const QString& format) {
    if (format == QT_TEXT) {
        return QStringLiteral(STLXWIN_MIME_TEXT);
    }

    return format == QT_IMAGE ? PNG : format;
}

// A URI list's URLs, one a line, without its comment lines
static QVariantList parsed_urls(const QByteArray& data) {
    QVariantList urls;
    for (const QByteArray& line : data.split('\n')) {
        QByteArray uri = line.trimmed();
        if (!uri.isEmpty() && !uri.startsWith('#')) {
            urls.append(QUrl::fromEncoded(uri));
        }
    }

    return urls;
}

static void add(QList<representation>& reps, const QString& mime_type, const QByteArray& data) {
    if (!data.isEmpty() && reps.size() < static_cast<qsizetype>(STLXWIN_OFFER_TYPES_MAX)) {
        reps.append({mime_type, data});
    }
}

// A format the desktop can carry by its own name: printable ASCII that fits the wire, since stlxdm ends a client whose
// offer names anything else
static bool is_wire_name(const QString& format) {
    QByteArray name = format.toLatin1();
    bool fits = !name.isEmpty() && name.size() < static_cast<qsizetype>(STLXWIN_MIME_MAX);
    return fits && std::all_of(name.begin(), name.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

// Formats the richer representations already carry, Qt's own internal ones, and the password hint, which is a flag
static bool is_carried(const QMimeData* data, const QString& format) {
    bool is_named = format == QT_TEXT || format == HTML || format == URI_LIST || format == PNG ||
                    format == PASSWORD_HINT;
    return is_named || format.startsWith(QStringLiteral("application/x-qt-")) ||
           (data->hasImage() && format.startsWith(QStringLiteral("image/")));
}

static QByteArray encoded_png(const QImage& image) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

QList<representation> representations(const QMimeData* data, bool includes_image) {
    QList<representation> reps;
    if (!data) {
        return reps;
    }

    if (includes_image && data->hasImage()) {
        add(reps, PNG, encoded_png(qvariant_cast<QImage>(data->imageData())));
    }

    if (data->hasUrls()) {
        add(reps, URI_LIST, data->data(URI_LIST));
    }

    if (data->hasHtml()) {
        add(reps, HTML, data->html().toUtf8());
    }

    if (data->hasText()) {
        add(reps, QStringLiteral(STLXWIN_MIME_TEXT), data->text().toUtf8());
    }

    for (const QString& format : data->formats()) {
        bool has_room = reps.size() < static_cast<qsizetype>(STLXWIN_OFFER_TYPES_MAX);
        if (has_room && !is_carried(data, format) && is_wire_name(format)) {
            reps.append({format, data->data(format)});
        }
    }

    return reps;
}

QStelluxMimeData::QStelluxMimeData(reader read)
    : m_read(std::move(read)) {
}

void QStelluxMimeData::show_types(const stlxwin_offer_types& types) {
    m_types.clear();
    for (uint32_t i = 0; i < types.count; i++) {
        m_types.append(QString::fromUtf8(types.types[i]));
    }

    m_fetched.clear();
}

QStringList QStelluxMimeData::formats() const {
    QStringList formats;
    for (const QString& mime_type : m_types) {
        if (mime_type == QLatin1String(STLXWIN_MIME_TEXT)) {
            formats.append(QT_TEXT);
            continue;
        }

        formats.append(mime_type);
        if (mime_type == PNG) {
            formats.append(QT_IMAGE);
        }
    }

    return formats;
}

bool QStelluxMimeData::hasFormat(const QString& format) const {
    return m_types.contains(desktop_type(format));
}

// Qt 6.11 turns a URI list's bytes into URLs through a QByteArrayView its QVariant cannot give, which leaves urls()
// empty, so a list asked for as URLs arrives parsed
QVariant QStelluxMimeData::retrieveData(const QString& format, QMetaType type) const {
    QString mime_type = desktop_type(format);
    if (!m_types.contains(mime_type)) {
        return QVariant();
    }

    QByteArray data = fetch(mime_type);
    if (data.isEmpty()) {
        return QVariant();
    }

    if (format == QT_IMAGE) {
        return QImage::fromData(data, "PNG");
    }

    bool wants_urls = type.id() == QMetaType::QVariantList || type.id() == QMetaType::QUrl;
    return format == URI_LIST && wants_urls ? QVariant(parsed_urls(data)) : QVariant(data);
}

// One paste often asks twice, for the data and then for its text, so a fetch answers every ask until the clipboard
// changes
QByteArray QStelluxMimeData::fetch(const QString& mime_type) const {
    auto kept = m_fetched.constFind(mime_type);
    if (kept != m_fetched.constEnd()) {
        return kept.value();
    }

    // The fetch waits for stlxdm's answer, queueing whatever events arrive meanwhile
    QByteArray data = m_read(mime_type.toUtf8().constData());
    QStelluxIntegration::instance()->schedule_queued_events();
    m_fetched.insert(mime_type, data);

    return data;
}

QStelluxClipboard::QStelluxClipboard(stlxwin_conn* connection)
    : m_connection(connection)
    , m_item([connection](const char* mime_type) {
        // Read in place and copied once, into the bytes Qt keeps
        const void* bytes = nullptr;
        long length = stlxwin_clipboard_map_type(connection, mime_type, &bytes);
        QByteArray data = length > 0 ? QByteArray(static_cast<const char*>(bytes), length) : QByteArray();
        stlxwin_clipboard_unmap(bytes, length > 0 ? static_cast<size_t>(length) : 0);
        return data;
    }) {
    // The watch waits for the clipboard's types, queueing whatever events arrive meanwhile
    if (stlxwin_clipboard_watch(m_connection) == 0) {
        read_types();
    }

    QStelluxIntegration::instance()->schedule_queued_events();
}

QMimeData* QStelluxClipboard::mimeData(QClipboard::Mode mode) {
    if (!supportsMode(mode)) {
        return nullptr;
    }

    return m_own ? m_own.get() : &m_item;
}

// Qt hands over the data, which the desktop keeps as up to four types and the app keeps whole while its copy lasts. A
// copy that fails leaves the clipboard unchanged, and data the app read back from the clipboard is never deleted.
void QStelluxClipboard::setMimeData(QMimeData* data, QClipboard::Mode mode) {
    bool is_ours = data == &m_item || data == m_own.get();
    if (!supportsMode(mode) || data == &m_item) {
        if (!is_ours) {
            delete data;
        }

        return;
    }

    m_copy++;
    bool is_private = data && data->data(PASSWORD_HINT) == "secret";
    if (data && data->hasImage()) {
        QImage image = qvariant_cast<QImage>(data->imageData());
        QList<representation> rest = representations(data, false);
        if (!is_ours) {
            m_own.reset(data);
        }

        emitChanged(mode);
        offer_image(std::move(image), std::move(rest), is_private, mode);
        return;
    }

    if (!offer(representations(data), is_private)) {
        if (!is_ours) {
            delete data;
        }

        return;
    }

    if (!is_ours) {
        m_own.reset(data);
    }

    emitChanged(mode);
}

// Offers `reps` as the desktop's clipboard and shows their types, false when the offer cannot be sent
bool QStelluxClipboard::offer(const QList<representation>& reps, bool is_private) {
    QByteArray names[STLXWIN_OFFER_TYPES_MAX];
    stlxwin_offer_rep wire[STLXWIN_OFFER_TYPES_MAX];
    stlxwin_offer_types offered = {};
    for (const representation& rep : reps) {
        uint32_t i = offered.count++;
        names[i] = rep.first.toUtf8();
        wire[i] = {names[i].constData(), rep.second.constData(), static_cast<size_t>(rep.second.size())};
        qstrncpy(offered.types[i], names[i].constData(), STLXWIN_MIME_MAX);
    }

    uint32_t flags = is_private ? STLXWIN_CLIPBOARD_PRIVATE : 0;
    if (stlxwin_clipboard_offer(m_connection, wire, offered.count, flags) != 0) {
        return false;
    }

    m_item.show_types(offered);
    return true;
}

// A copied image's PNG is encoded on a pool thread, so a large image never holds up the app, and offered from the GUI
// thread, which libstlxwin calls stay on, unless a newer copy or another app's change came first. The app's own
// pastes read its data meanwhile, and a copy whose offer fails gives the app the desktop's clipboard back.
void QStelluxClipboard::offer_image(QImage image, QList<representation> rest, bool is_private, QClipboard::Mode mode) {
    std::weak_ptr<bool> alive = m_alive;
    uint64_t copy = m_copy;
    QThreadPool::globalInstance()->start([this, alive, image, rest, is_private, mode, copy]() {
        QByteArray png = encoded_png(image);
        QMetaObject::invokeMethod(qApp, [this, alive, png, rest, is_private, mode, copy]() {
            if (!alive.lock() || copy != m_copy) {
                return;
            }

            QList<representation> reps;
            add(reps, PNG, png);
            for (const representation& rep : rest) {
                add(reps, rep.first, rep.second);
            }

            if (!offer(reps, is_private)) {
                m_own.reset();
                emitChanged(mode);
            }
        }, Qt::QueuedConnection);
    });
}

bool QStelluxClipboard::supportsMode(QClipboard::Mode mode) const {
    return mode == QClipboard::Clipboard;
}

// The app's own copy came back as the clipboard's change, which setMimeData already told Qt of, and any other change
// drops the app's data
void QStelluxClipboard::changed() {
    read_types();
    if (m_own && stlxwin_clipboard_is_own(m_connection)) {
        return;
    }

    m_copy++;
    m_own.reset();
    emitChanged(QClipboard::Clipboard);
}

void QStelluxClipboard::read_types() {
    stlxwin_offer_types types;
    if (stlxwin_clipboard_types(m_connection, &types) == 0) {
        m_item.show_types(types);
    }
}
