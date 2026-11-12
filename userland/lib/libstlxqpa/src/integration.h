#ifndef STLXQPA_INTEGRATION_H
#define STLXQPA_INTEGRATION_H

#include <QHash>
#include <QList>
#include <QPoint>
#include <memory>
#include <qpa/qplatformclipboard.h>
#include <qpa/qplatformcursor.h>
#include <qpa/qplatformfontdatabase.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformservices.h>

#include <stlxwin/stlxwin.h>

class QSocketNotifier;
class QStelluxDrag;
class QStelluxWindow;

// Qt's base dots per inch, reported by every screen so Qt scales nothing and a point is 4/3 of a pixel
constexpr qreal SCREEN_LOGICAL_DPI = 96;

// The pointer's shape, and where it is as the newest pointer or drop event found it, which Qt's own position misses
// while a drag runs and item views read to scroll near their edges
class QStelluxCursor : public QPlatformCursor {
public:
    void changeCursor(QCursor* cursor, QWindow* window) override;
    QPoint pos() const override;
};

// Qt's screen for one output that stlxdm's OUTPUTS lists, or one screen at the
// size the handshake reported from a display manager that lists none. Every
// screen is a virtual sibling of every other, so Qt moves a removed screen's
// windows to its primary screen still shown, rather than making them again.
class QStelluxScreen : public QPlatformScreen {
public:
    QStelluxScreen(uint32_t output, const QRect& geometry, const QRect& available_geometry, qreal refresh_rate);

    uint32_t output() const;
    void update(const QRect& geometry, const QRect& available_geometry, qreal refresh_rate);

    QRect geometry() const override;
    QRect availableGeometry() const override;
    int depth() const override;
    QImage::Format format() const override;
    QDpi logicalDpi() const override;
    qreal refreshRate() const override;
    QList<QPlatformScreen*> virtualSiblings() const override;
    QPlatformCursor* cursor() const override;

private:
    uint32_t m_output;
    QRect m_geometry;
    QRect m_available_geometry;
    qreal m_refresh_rate;
    std::unique_ptr<QStelluxCursor> m_cursor;
};

/**
 * The Qt platform integration for the Stellux display manager. It owns the
 * libstlxwin connection, turns its events into Qt window system events, and
 * paints every window through the raster engine into stlxwin buffers.
 */
class QStelluxIntegration : public QPlatformIntegration {
public:
    QStelluxIntegration();
    ~QStelluxIntegration() override;

    static QStelluxIntegration* instance();

    bool is_connected() const;
    stlxwin_conn* connection() const;
    void add_window(stlxwin_window* handle, QStelluxWindow* window);
    void remove_window(stlxwin_window* handle);
    QList<QStelluxWindow*> windows() const;
    QList<QPlatformScreen*> screens() const;
    QStelluxScreen* screen_of(uint32_t output) const;
    void schedule_queued_events();
    QStelluxDrag* stellux_drag() const;
    void note_pointer(const QPoint& global);
    QPoint pointer() const;

    void initialize() override;
    bool hasCapability(Capability capability) const override;
    QPlatformWindow* createPlatformWindow(QWindow* window) const override;
    QPlatformBackingStore* createPlatformBackingStore(QWindow* window) const override;
    QAbstractEventDispatcher* createEventDispatcher() const override;
    QPlatformFontDatabase* fontDatabase() const override;
    QPlatformClipboard* clipboard() const override;
    QPlatformDrag* drag() const override;
    QPlatformServices* services() const override;
    QStringList themeNames() const override;
    QPlatformTheme* createPlatformTheme(const QString& name) const override;

private:
    void read_connection();
    void dispatch_queued_events();
    void update_screens();

    stlxwin_conn* m_connection = nullptr;
    QList<QStelluxScreen*> m_screens;
    QSocketNotifier* m_notifier = nullptr;
    QHash<stlxwin_window*, QStelluxWindow*> m_windows;
    QPoint m_pointer;
    mutable std::unique_ptr<QPlatformFontDatabase> m_font_database;
    mutable std::unique_ptr<QPlatformClipboard> m_clipboard;
    mutable std::unique_ptr<QStelluxDrag> m_drag;
    mutable std::unique_ptr<QPlatformServices> m_services;
};

#endif
