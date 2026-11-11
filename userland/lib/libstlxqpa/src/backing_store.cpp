#include "backing_store.h"
#include "window.h"

#include <QPainter>
#include <QWindow>
#include <cstring>

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

// Moves the scrolled pixels inside the image, so Qt repaints only the strip that scrolled into view
bool QStelluxBackingStore::scroll(const QRegion& area, int dx, int dy) {
    const QRect bounds = m_image.rect();
    const QRect from = area.boundingRect().intersected(bounds).intersected(bounds.translated(-dx, -dy));
    if (from.isEmpty()) {
        return !m_image.isNull();
    }

    const QRect to = from.translated(dx, dy);
    const qsizetype stride = m_image.bytesPerLine();
    const qsizetype pixel_bytes = m_image.depth() / 8;
    const size_t row_bytes = static_cast<size_t>(from.width() * pixel_bytes);

    uchar* bits = m_image.bits();
    const uchar* source = bits + from.top() * stride + from.left() * pixel_bytes;
    uchar* target = bits + to.top() * stride + to.left() * pixel_bytes;

    // Rows move starting from the edge they travel toward, so each is read before another lands on it
    for (int i = 0; i < from.height(); i++) {
        int row = dy > 0 ? from.height() - 1 - i : i;
        std::memmove(target + row * stride, source + row * stride, row_bytes);
    }

    return true;
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
