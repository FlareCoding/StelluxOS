#ifndef STLXQPA_BACKING_STORE_H
#define STLXQPA_BACKING_STORE_H

#include <QImage>
#include <qpa/qplatformbackingstore.h>

// The raster engine paints into an XRGB image, the same format stlxwin buffers use
class QStelluxBackingStore : public QPlatformBackingStore {
public:
    explicit QStelluxBackingStore(QWindow* window);

    QPaintDevice* paintDevice() override;
    void flush(QWindow* window, const QRegion& region, const QPoint& offset) override;
    void resize(const QSize& size, const QRegion& static_contents) override;
    QImage toImage() const override;

private:
    QImage m_image;
};

#endif
