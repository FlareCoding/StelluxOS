#ifndef STLXQPA_CLIPBOARD_H
#define STLXQPA_CLIPBOARD_H

#include <QHash>
#include <QMimeData>
#include <QStringList>
#include <qpa/qplatformclipboard.h>

#include <stlxwin/stlxwin.h>

// The desktop clipboard's item, fetched one representation at a time as Qt asks for it and kept until it changes
class QStelluxMimeData : public QMimeData {
public:
    explicit QStelluxMimeData(stlxwin_conn* connection);

    // Forgets what was fetched, since the clipboard now holds `types`
    void show_types(const stlxwin_clip_types& types);

    QStringList formats() const override;
    bool hasFormat(const QString& format) const override;

protected:
    QVariant retrieveData(const QString& format, QMetaType type) const override;

private:
    QByteArray fetch(const QString& mime_type) const;

    stlxwin_conn* m_connection;
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
