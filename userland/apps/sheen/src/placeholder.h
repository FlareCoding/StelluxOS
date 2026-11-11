#ifndef SHEEN_PLACEHOLDER_H
#define SHEEN_PLACEHOLDER_H

/* What a tab shows instead of text for a file that looks binary: why, and the ways to open it as text anyway. The
 * question sits in the tab, so the user sees which file it is about and Ctrl W and Ctrl Shift T work on it. */
#include <QByteArray>
#include <QWidget>

#include <functional>

class binary_placeholder : public QWidget {
public:
    /* `utf16_encoding` names UTF-16 LE or BE when the file's NULs fall on every other byte, empty otherwise */
    binary_placeholder(QWidget* parent, const QString& name, const QByteArray& utf16_encoding);

    std::function<void(const QByteArray& encoding)> on_open_as;
    std::function<void()> on_close;

protected:
    void paintEvent(QPaintEvent* event) override;
};

#endif
