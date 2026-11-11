#ifndef SHEEN_BANNER_H
#define SHEEN_BANNER_H

/* A strip under the tab strip that tells about the current tab's file, a change made outside Sheen or a failure,
 * with the actions that resolve it. A change can be dismissed, a failure stays until it is resolved. */
#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

class QHBoxLayout;
class QLabel;

enum class banner_kind { CHANGE, READ_ONLY, FAILURE };

struct banner_action {
    QString label;
    std::function<void()> run;
};

struct banner_message {
    banner_kind kind;
    QString subject; // the file the message is about, drawn in the primary text color
    QString text;    // what follows the subject
    QList<banner_action> actions;
};

class banner : public QWidget {
public:
    explicit banner(QWidget* parent);

    void show_message(const banner_message& message);

    /* Runs when the close glyph dismisses a change */
    std::function<void()> on_dismissed;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    banner_kind m_kind = banner_kind::CHANGE;
    QLabel* m_text;
    QHBoxLayout* m_actions;
    QWidget* m_close;
};

#endif
