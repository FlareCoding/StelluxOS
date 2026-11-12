#ifndef STLXQPA_CLIPBOARD_H
#define STLXQPA_CLIPBOARD_H

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMimeData>
#include <QPair>
#include <QStringList>
#include <qpa/qplatformclipboard.h>

#include <functional>
#include <memory>

#include <stlxwin/stlxwin.h>

// One representation of an item as a MIME type and its bytes
using representation = QPair<QString, QByteArray>;

// What a copy or a drag offers of Qt's data, the richest first: an image as PNG unless `includes_image` is false, files,
// HTML, then text, and then the app's own formats while room is left, empty markers included
QList<representation> representations(const QMimeData* data, bool includes_image = true);

// An item of the desktop's, the clipboard's or a drop's, fetched one representation at a time as Qt asks for it and
// kept until its types change
class QStelluxMimeData : public QMimeData {
public:
    // Reads one representation, empty when there is none or the read fails
    using reader = std::function<QByteArray(const char* mime_type)>;

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

    // Takes the types of a new copy, keeping the app's own data while the copy is the app's own
    void changed();

private:
    void read_types();
    bool offer(const QList<representation>& reps, bool is_private);
    void offer_image(QImage image, QList<representation> rest, bool is_private, QClipboard::Mode mode);

    stlxwin_conn* m_connection;
    QStelluxMimeData m_item;

    // Counts the app's copies and other apps' changes, so a PNG still encoding for an older copy is never offered
    uint64_t m_copy = 0;

    // Checked by the encoder's answer before it touches the clipboard, which may be gone by then
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    // The app's own latest copy, which answers its own pastes while the clipboard holds it, as Qt's contract keeps a
    // copy's data within its app, formats the desktop does not carry included
    std::unique_ptr<QMimeData> m_own;
};

#endif
