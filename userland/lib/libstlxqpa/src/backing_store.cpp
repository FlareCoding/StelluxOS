#include "backing_store.h"
#include "window.h"

#include <QWindow>

QStelluxBackingStore::QStelluxBackingStore(QWindow* window)
    : QPlatformBackingStore(window) {
}

QPaintDevice* QStelluxBackingStore::paintDevice() {
    return &m_image;
}

void QStelluxBackingStore::flush(QWindow* window, const QRegion& region, const QPoint& offset) {
    auto* platform_window = static_cast<QStelluxWindow*>(window->handle());
    if (platform_window) {
        platform_window->present(m_image, region.translated(offset));
    }
}

void QStelluxBackingStore::resize(const QSize& size, const QRegion& static_contents) {
    Q_UNUSED(static_contents);

    if (m_image.size() != size) {
        m_image = QImage(size, QImage::Format_RGB32);
    }
}

QImage QStelluxBackingStore::toImage() const {
    return m_image;
}
