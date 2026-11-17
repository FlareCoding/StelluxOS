#ifndef SHEEN_EMPTY_STATE_H
#define SHEEN_EMPTY_STATE_H

/* What a window without tabs shows on its glass: the Sheen icon, the next actions with their keys and this session's
 * files, which a click reopens. Up and Down move between the rows and Enter runs the lit one. */
#include <QPixmap>
#include <QStringList>
#include <QWidget>

#include <functional>

/* Mock 09's glyphs before the rows: a page, a folder, a clock and a terminal */
enum class row_glyph { DOC, FOLDER, CLOCK, TERMINAL };

struct empty_action {
    QString label;
    QStringList keys;
    std::function<void()> run;
    row_glyph glyph;
};

class empty_state : public QWidget {
public:
    explicit empty_state(QWidget* parent);

    /* Lists `actions` as rows with their keycaps and up to five of `files`, newest first, under them */
    void set_entries(const QList<empty_action>& actions, const QStringList& files);

    std::function<void(const QString& path)> on_file_chosen;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    int icon_room() const;
    int row_count() const;
    int block_top() const;
    QRect row_rect(int row) const;
    int row_at(const QPoint& point) const;
    void run_row(int row);
    void light_row(int row);

    QList<empty_action> m_actions;
    QStringList m_files;
    QPixmap m_icon;
    int m_lit = 0;
};

#endif
