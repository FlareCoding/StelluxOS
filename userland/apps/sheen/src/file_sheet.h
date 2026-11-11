#ifndef SHEEN_FILE_SHEET_H
#define SHEEN_FILE_SHEET_H

/* The Open and Save As sheets: places, the folder's crumb and entries in File Manager's rows, keyboard first. Typing
 * narrows the folder, Enter opens a file or enters a folder, and Ctrl L or a leading / turns the crumb into a path. */
#include "encoding.h"
#include "sheet.h"

#include <QDateTime>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;
class QTreeView;
class crumb_bar;
class folder_model;
class name_match_delegate;

enum class file_sheet_mode { OPEN, SAVE_AS };

/* A file opened this session and when, as Recent lists it */
struct recent_file {
    QString path;
    QDateTime opened;
};

struct open_request {
    QStringList paths;
    QByteArray encoding; // empty to detect
};

struct save_request {
    QString path;
    file_encoding encoding;
    int eol_mode;
};

class file_sheet : public sheet {
public:
    /* `recent` lists the files opened this session, newest first */
    file_sheet(QWidget* window, file_sheet_mode mode, const QString& folder, const QList<recent_file>& recent);

    /* Starts on `current_path` when the folder shown holds it, on the first row otherwise */
    bool choose_to_open(open_request* request, const QString& current_path);

    /* Replaces the header's own title, as when several files are named in turn */
    void set_title(const QString& title);

    /* A dim note beside the title, such as where a suggested name comes from */
    void set_note(const QString& note);

    /* `name` is the suggested file name, with the part before its extension selected */
    bool choose_to_save(const QString& name, const file_encoding& encoding, int eol_mode, save_request* request);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void cancel() override;

private:
    void show_folder(const QString& path, bool is_new_step);
    void show_recent();
    void set_filter(const QString& filter);
    void step_history(int direction);
    void activate_current();
    void accept_path_field();
    void edit_path(const QString& text);
    void show_crumb();
    bool complete_path();
    void fit_columns();
    void accept();
    void refresh_primary();
    void point_at_start();
    void ask_to_replace(const QString& path);
    QString current_folder() const;
    QStringList selected_files() const;

    file_sheet_mode m_mode;
    folder_model* m_model;
    name_match_delegate* m_matches;
    QTreeView* m_list = nullptr;
    QListWidget* m_places;
    crumb_bar* m_crumb = nullptr;
    QLineEdit* m_path = nullptr;
    QLineEdit* m_name = nullptr;
    QLabel* m_title;
    QLabel* m_note;
    QLabel* m_filter_hint;
    QLabel* m_footer_note;
    QComboBox* m_encoding;
    QComboBox* m_eol = nullptr;
    QPushButton* m_primary;
    QToolButton* m_back;
    QToolButton* m_forward;
    QStringList m_history;
    int m_history_index = -1;
    QList<recent_file> m_recent;
    QString m_replace_path;
    QString m_current_path;
    open_request m_open;
    save_request m_save;
};

/* Why a path that exists cannot open as text, by its kind, such as "it is a device, not a file". Empty for a
 * regular file or a path that does not exist */
QString refusal_by_kind(const QString& path);

#endif
