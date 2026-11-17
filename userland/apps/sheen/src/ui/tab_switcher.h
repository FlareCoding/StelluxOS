#ifndef SHEEN_TAB_SWITCHER_H
#define SHEEN_TAB_SWITCHER_H

/* The recent tabs, most recent first, in a panel at the top center of the window while Ctrl stays held after a
 * second Tab. The window walks the lit row and opens it when Ctrl comes up. */
#include <QList>
#include <QStringList>
#include <QWidget>

class tab_switcher : public QWidget {
public:
    explicit tab_switcher(QWidget* window);

    /* Shows `names` with the row at `lit` lit, placed `top` px down the window */
    void show_list(const QStringList& names, const QList<bool>& unsaved, int lit, int top);
    void set_lit(int lit);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QStringList m_names;
    QList<bool> m_unsaved;
    int m_lit = 0;
};

#endif
