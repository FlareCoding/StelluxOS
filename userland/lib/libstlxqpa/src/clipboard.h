#ifndef STLXQPA_CLIPBOARD_H
#define STLXQPA_CLIPBOARD_H

#include <QMimeData>
#include <qpa/qplatformclipboard.h>

#include <stlxwin/stlxwin.h>

// The desktop's text clipboard, which stlxdm holds for every client
class QStelluxClipboard : public QPlatformClipboard {
public:
    explicit QStelluxClipboard(stlxwin_conn* connection);

    QMimeData* mimeData(QClipboard::Mode mode) override;
    void setMimeData(QMimeData* data, QClipboard::Mode mode) override;
    bool supportsMode(QClipboard::Mode mode) const override;

private:
    stlxwin_conn* m_connection;
    QMimeData m_fetched;
};

#endif
