#ifndef STLXQPA_CLIPBOARD_H
#define STLXQPA_CLIPBOARD_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMimeData>
#include <QPair>
#include <QStringList>
#include <qpa/qplatformclipboard.h>

#include <functional>

#include <stlxwin/stlxwin.h>

// One representation of an item as a MIME type and its bytes
using representation = QPair<QString, QByteArray>;

// What a copy or a drag offers of Qt's data, the richest first: an image as PNG, files, HTML, then text
QList<representation> representations(const QMimeData* data);

// An item of the desktop's, the clipboard's or a drop's, fetched one representation at a time as Qt asks for it and
// kept until its types change
class QStelluxMimeData : public QMimeData {
public:
    // Reads one representation into a malloc'd buffer, returning its length, 0 when there is none, or -1
    using reader = std::function<long(const char* mime_type, char** out_data)>;

    explicit QStelluxMimeData(reader read);

    // Forgets what was fetched, since the item now holds `types`
    void show_types(const stlxwin_offer_types& types);

    QStringList formats() const override;
    bool hasFormat(const QString& format) const override;

protected:
    QVariant retrieveData(const QString& format, QMetaType type) const override;

private:
    QByteArray fetch(const QString& mime_type) const;

    reader m_read;
    QStringList m_types;
    mutable QHash<QString, QByteArray> m_fetched;
};

// The desktop's clipboard, which stlxdm holds for every client as items of up to four types
class QStelluxClipboard : public QPlatformClipboard {
public:
    explicit QStelluxClipboard(stlxwin_conn* connection);

    QMimeData* mimeData(QClipboard::Mode mode) override;
    void setMimeData(QMimeData* data, QClipboard::Mode mode) override;
    bool supportsMode(QClipboard::Mode mode) const override;

    // Takes the types of another client's copy
    void changed();

private:
    void read_types();

    stlxwin_conn* m_connection;
    QStelluxMimeData m_item;
};

#endif
