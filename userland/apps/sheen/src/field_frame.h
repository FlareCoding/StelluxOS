#ifndef SHEEN_FIELD_FRAME_H
#define SHEEN_FIELD_FRAME_H

#include <QWidget>

class QHBoxLayout;
class QLineEdit;
class QToolButton;

/* A recessed field holding its toggles, underlined in amber while focused and in the danger color while its contents
 * cannot be read */
class field_frame : public QWidget {
public:
    field_frame(QLineEdit* edit, QWidget* parent);

    void add_option(QToolButton* option);
    void show_magnifier();
    void set_bad(bool is_bad);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QLineEdit* m_edit;
    QHBoxLayout* m_layout;
    bool m_is_bad = false;
    bool m_has_magnifier = false;
};

#endif
