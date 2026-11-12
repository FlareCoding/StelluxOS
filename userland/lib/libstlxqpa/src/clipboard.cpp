#include "clipboard.h"
#include "integration.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QPair>
#include <QUrl>

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

QList<representation> representations(const QMimeData* data) {
    QList<representation> reps;
    if (!data) {
        return reps;
    }

    if (data->hasImage()) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        qvariant_cast<QImage>(data->imageData()).save(&buffer, "PNG");
        add(reps, PNG, png);
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
    char* bytes = nullptr;
    long length = m_read(mime_type.toUtf8().constData(), &bytes);
    QStelluxIntegration::instance()->schedule_queued_events();

    QByteArray data = length > 0 ? QByteArray(bytes, length) : QByteArray();
    std::free(bytes);
    m_fetched.insert(mime_type, data);

    return data;
}

QStelluxClipboard::QStelluxClipboard(stlxwin_conn* connection)
    : m_connection(connection)
    , m_item([connection](const char* mime_type, char** out_data) {
        return stlxwin_clipboard_get_type(connection, mime_type, out_data);
    }) {
    // The watch waits for the clipboard's types, queueing whatever events arrive meanwhile
    if (stlxwin_clipboard_watch(m_connection) == 0) {
        read_types();
    }

    QStelluxIntegration::instance()->schedule_queued_events();
}

QMimeData* QStelluxClipboard::mimeData(QClipboard::Mode mode) {
    return supportsMode(mode) ? &m_item : nullptr;
}

// Qt hands over the data, which the desktop keeps as up to four types. A copy that fails leaves the clipboard
// unchanged.
void QStelluxClipboard::setMimeData(QMimeData* data, QClipboard::Mode mode) {
    if (!supportsMode(mode)) {
        delete data;
        return;
    }

    QList<representation> reps = representations(data);
    bool is_private = data && data->data(PASSWORD_HINT) == "secret";
    delete data;

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
        return;
    }

    m_item.show_types(offered);
    emitChanged(mode);
}

bool QStelluxClipboard::supportsMode(QClipboard::Mode mode) const {
    return mode == QClipboard::Clipboard;
}

void QStelluxClipboard::changed() {
    read_types();
    emitChanged(QClipboard::Clipboard);
}

void QStelluxClipboard::read_types() {
    stlxwin_offer_types types;
    if (stlxwin_clipboard_types(m_connection, &types) == 0) {
        m_item.show_types(types);
    }
}
