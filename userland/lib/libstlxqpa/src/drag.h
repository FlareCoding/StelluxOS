#ifndef STLXQPA_DRAG_H
#define STLXQPA_DRAG_H

#include <QPointer>
#include <QWindow>
#include <qpa/qplatformdrag.h>

#include <stlxwin/stlxwin.h>

class QEventLoop;

// The STLXWIN_DND_* actions of Qt's drop actions, and Qt's of the desktop's
uint32_t to_stlxwin_actions(Qt::DropActions actions);
Qt::DropActions to_qt_actions(uint32_t actions);

// The desktop's drag, which stlxdm carries between windows and apps, in place of Qt's in-process QSimpleDrag
class QStelluxDrag : public QPlatformDrag {
public:
    explicit QStelluxDrag(stlxwin_conn* connection);

    // A press on one of the app's windows, which the next drag starts from
    void note_press(QWindow* window, uint32_t serial);

    // Ends the running drag once stlxdm reports that it finished or was cancelled
    void handle_state(const stlxwin_event& event);

    Qt::DropAction drag(QDrag* drag) override;
    void cancelDrag() override;
    Qt::DropAction defaultAction(Qt::DropActions possible_actions, Qt::KeyboardModifiers modifiers) const override;

private:
    stlxwin_conn* m_connection;
    QPointer<QWindow> m_press_window;
    uint32_t m_press_serial = 0;
    uint32_t m_drag = 0;            // the running drag's serial, 0 for none
    QEventLoop* m_loop = nullptr;   // runs while the drag does
    Qt::DropAction m_result = Qt::IgnoreAction;
};

#endif
