#include "backing_store.h"
#include "window.h"

#include <QPainter>
#include <QWindow>

QStelluxBackingStore::QStelluxBackingStore(QWindow* window)
    : QPlatformBackingStore(window) {
}

QPaintDevice* QStelluxBackingStore::paintDevice() {
    return &m_image;
}

// Translucent paint would stack on the last frame's pixels, so each painted region starts transparent
void QStelluxBackingStore::beginPaint(const QRegion& region) {
    if (!m_image.hasAlphaChannel()) {
        return;
    }

    QPainter painter(&m_image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect& rect : region) {
        painter.fillRect(rect, Qt::transparent);
    }
}

void QStelluxBackingStore::flush(QWindow* window, const QRegion& region, const QPoint& offset) {
    auto* platform_window = static_cast<QStelluxWindow*>(window->handle());
    if (platform_window) {
        platform_window->present(m_image, region.translated(offset));
    }
}

void QStelluxBackingStore::resize(const QSize& size, const QRegion& static_contents) {
    Q_UNUSED(static_contents);

    auto* platform_window = static_cast<QStelluxWindow*>(window()->handle());
    bool shows_glass = platform_window && platform_window->shows_glass();
    QImage::Format format = shows_glass ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
    if (m_image.size() != size || m_image.format() != format) {
        m_image = QImage(size, format);
    }
}

QImage QStelluxBackingStore::toImage() const {
    return m_image;
}
