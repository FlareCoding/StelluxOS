#include "clipboard.h"
#include "integration.h"

#include <cstdlib>

QStelluxClipboard::QStelluxClipboard(stlxwin_conn* connection)
    : m_connection(connection) {
}

QMimeData* QStelluxClipboard::mimeData(QClipboard::Mode mode) {
    if (!supportsMode(mode)) {
        return nullptr;
    }

    // The fetch waits for stlxdm's answer, queueing whatever events arrive meanwhile
    char* text = nullptr;
    long length = stlxwin_clipboard_get(m_connection, &text);
    QStelluxIntegration::instance()->schedule_queued_events();

    m_fetched.setText(length > 0 ? QString::fromUtf8(text, length) : QString());
    std::free(text);

    return &m_fetched;
}

// Qt hands over the data, which the desktop keeps as text only
void QStelluxClipboard::setMimeData(QMimeData* data, QClipboard::Mode mode) {
    if (!supportsMode(mode)) {
        delete data;
        return;
    }

    QByteArray text = data ? data->text().toUtf8() : QByteArray();
    stlxwin_clipboard_set(m_connection, text.constData(), static_cast<size_t>(text.size()));
    delete data;

    emitChanged(mode);
}

bool QStelluxClipboard::supportsMode(QClipboard::Mode mode) const {
    return mode == QClipboard::Clipboard;
}
