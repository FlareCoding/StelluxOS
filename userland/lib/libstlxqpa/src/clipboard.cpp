#include "clipboard.h"
#include "integration.h"

#include <cstdlib>

QStelluxClipboard::QStelluxClipboard(stlxwin_conn* connection)
    : m_connection(connection) {
}

// One paste often asks twice, for the data and then for its text, so a fetch answers every ask until control returns
// to the event loop
QMimeData* QStelluxClipboard::mimeData(QClipboard::Mode mode) {
    if (!supportsMode(mode)) {
        return nullptr;
    }

    if (m_is_fetch_current) {
        return &m_fetched;
    }

    // The fetch waits for stlxdm's answer, queueing whatever events arrive meanwhile
    char* text = nullptr;
    long length = stlxwin_clipboard_get(m_connection, &text);
    QStelluxIntegration::instance()->schedule_queued_events();

    m_fetched.setText(length > 0 ? QString::fromUtf8(text, length) : QString());
    std::free(text);

    m_is_fetch_current = true;
    QMetaObject::invokeMethod(&m_fetched, [this] { m_is_fetch_current = false; }, Qt::QueuedConnection);

    return &m_fetched;
}

// Qt hands over the data, which the desktop keeps as text only. A copy that fails leaves the clipboard unchanged.
void QStelluxClipboard::setMimeData(QMimeData* data, QClipboard::Mode mode) {
    if (!supportsMode(mode)) {
        delete data;
        return;
    }

    QByteArray text = data ? data->text().toUtf8() : QByteArray();
    int result = stlxwin_clipboard_set(m_connection, text.constData(), static_cast<size_t>(text.size()));
    delete data;

    if (result == 0) {
        m_is_fetch_current = false;
        emitChanged(mode);
    }
}

bool QStelluxClipboard::supportsMode(QClipboard::Mode mode) const {
    return mode == QClipboard::Clipboard;
}
