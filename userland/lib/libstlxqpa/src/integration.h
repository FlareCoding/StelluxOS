#ifndef STLXQPA_INTEGRATION_H
#define STLXQPA_INTEGRATION_H

#include <QHash>
#include <memory>
#include <qpa/qplatformclipboard.h>
#include <qpa/qplatformcursor.h>
#include <qpa/qplatformfontdatabase.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformservices.h>

#include <stlxwin/stlxwin.h>

class QSocketNotifier;
class QStelluxWindow;

class QStelluxCursor : public QPlatformCursor {
public:
    void changeCursor(QCursor* cursor, QWindow* window) override;
};

// The one screen stlxdm reports. Positions are kept relative to each toplevel,
// which is all stlxdm needs to place a popup.
class QStelluxScreen : public QPlatformScreen {
public:
    explicit QStelluxScreen(const QSize& size);

    QRect geometry() const override;
    int depth() const override;
    QImage::Format format() const override;
    QPlatformCursor* cursor() const override;

private:
    QRect m_geometry;
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
    void schedule_queued_events();

    void initialize() override;
    bool hasCapability(Capability capability) const override;
    QPlatformWindow* createPlatformWindow(QWindow* window) const override;
    QPlatformBackingStore* createPlatformBackingStore(QWindow* window) const override;
    QAbstractEventDispatcher* createEventDispatcher() const override;
    QPlatformFontDatabase* fontDatabase() const override;
    QPlatformClipboard* clipboard() const override;
    QPlatformServices* services() const override;
    QStringList themeNames() const override;
    QPlatformTheme* createPlatformTheme(const QString& name) const override;

private:
    void read_connection();
    void dispatch_queued_events();

    stlxwin_conn* m_connection = nullptr;
    QStelluxScreen* m_screen = nullptr;
    QSocketNotifier* m_notifier = nullptr;
    QHash<stlxwin_window*, QStelluxWindow*> m_windows;
    mutable std::unique_ptr<QPlatformFontDatabase> m_font_database;
    mutable std::unique_ptr<QPlatformClipboard> m_clipboard;
    mutable std::unique_ptr<QPlatformServices> m_services;
};

#endif
