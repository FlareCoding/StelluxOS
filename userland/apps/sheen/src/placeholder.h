#ifndef SHEEN_PLACEHOLDER_H
#define SHEEN_PLACEHOLDER_H

/* What a tab shows instead of text: why a file that looks binary waits and the ways to open it as text anyway, or
 * how much of a large file has arrived. Either sits in the tab, so Ctrl W and Ctrl Shift T work on it. */
#include <QByteArray>
#include <QWidget>

#include <functional>

class QLabel;

class binary_placeholder : public QWidget {
public:
    /* `utf16_encoding` names UTF-16 LE or BE when the file's NULs fall on every other byte, empty otherwise */
    binary_placeholder(QWidget* parent, const QString& name, const QByteArray& utf16_encoding);

    std::function<void(const QByteArray& encoding)> on_open_as;
    std::function<void()> on_close;

protected:
    void paintEvent(QPaintEvent* event) override;
};

/* Which file is opening, a bar and how many of its megabytes have arrived, with Cancel, which Esc presses too */
class loading_placeholder : public QWidget {
public:
    loading_placeholder(QWidget* parent, const QString& name, qint64 size);

    /* `done` of `total` steps, shown as that share of the file's size */
    void set_progress(qint64 done, qint64 total);

    std::function<void()> on_cancel;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QLabel* m_amount;
    QWidget* m_bar;
    qint64 m_size;
    double m_share = 0;
};

#endif
