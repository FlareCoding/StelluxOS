#ifndef SHEEN_DOCUMENT_H
#define SHEEN_DOCUMENT_H

/* One open file: the Scintilla view that edits it in UTF-8, its language, and the record of the file on disk with
 * the encoding and line ending it keeps. A document without a path has never been saved. */
#include "banner.h"
#include "encoding.h"
#include "settings.h"

#include <QDateTime>
#include <QString>

#include <Scintilla.h>

#include <optional>

class ScintillaEditBase;
class QWidget;
struct language;

struct eol_counts {
    qsizetype crlf = 0;
    qsizetype cr = 0;
    qsizetype lf = 0;
};

struct indentation {
    bool uses_tabs;
    int width;
};

struct file_record {
    QString path;
    file_encoding encoding = { QByteArrayLiteral("UTF-8"), false };
    int eol_mode = SC_EOL_LF;    // the line ending new lines take
    bool has_mixed_eols = false;
    eol_counts eols;             // as the file was read
    qint64 size = -1;            // at the last load or save, to notice a change made elsewhere
    QDateTime modified;
};

enum class disk_state { SAME, CHANGED, DELETED };

/* Code is for source, scripts and config, Text for notes, prose and plain files */
enum class edit_mode { CODE, TEXT };

/* Where the caret, the selection and the scroll stood, so a reopened tab returns to them */
struct view_state {
    sptr_t caret = 0;
    sptr_t anchor = 0;
    sptr_t first_line = 0;
    int wrap_mode = SC_WRAP_NONE;
};

/* The first character an encoding cannot hold, as a refused save names it */
struct unencodable_char {
    sptr_t line = 0; // counted from one, zero when every character was held
    char32_t code = 0;
};

class document {
public:
    /* `untitled_number` names the document until it has a path or a first line, "Untitled 2" for 2 */
    document(QWidget* parent, int untitled_number, const sheen_settings& settings);
    ~document();

    document(const document&) = delete;
    document& operator=(const document&) = delete;

    /* Keeps the bytes of a file that looks binary unopened behind `placeholder`, which the document then owns, until
     * `open_held` decodes them */
    void hold(const QString& path, const QByteArray& bytes, QWidget* placeholder);
    bool open_held(const QByteArray& encoding);
    QWidget* placeholder() const { return m_placeholder; }

    /* Fills the view with `bytes`, the contents of the file at `path`, decoded as `encoding`, or as detected when
     * `encoding` is empty. Returns false when the bytes are not valid in `encoding` */
    bool load(const QString& path, const QByteArray& bytes, const QByteArray& encoding);

    /* Rereads the file from disk as `encoding`. Returns false with `error` set when it does not decode as one */
    bool reload_as(const QByteArray& encoding, QString* error);

    /* Replaces the text with the file's current contents as one undo step, keeping the caret's line and column */
    bool reload_from_disk(QString* error);

    /* Writes the text to `path` through a temporary file renamed over it, so a failed save leaves the file as it was.
     * Returns false with `error` set, or with `unencodable` naming the first character the encoding cannot hold */
    bool save(const QString& path, QString* error, unencodable_char* unencodable);

    /* Takes over `scintilla_document`, a Scintilla document with its text, undo and change history, and drops the
     * reference the caller held on it */
    void adopt(sptr_t scintilla_document, const file_record& file, const language* lang);

    /* Adds a reference to the Scintilla document and returns it, so it outlives this view */
    sptr_t keep_scintilla_document() const;

    void set_encoding(const file_encoding& encoding);

    /* A language the user did not pick a mode for takes its own mode, Code for every lexed language but Markdown */
    void set_language(const language* lang);

    /* Resets the view to the mode's defaults. A chosen mode stays when the file's language changes */
    void set_mode(edit_mode mode, bool is_chosen);
    edit_mode mode() const { return m_mode; }

    /* Keeps Text mode's reading width centered as the view's width changes */
    void fit_text_width();

    /* Code mode marks function names on the lines in view, so each edit, scroll or showing marks them again */
    void refresh_function_names();
    void set_eol_mode(int eol_mode);
    void convert_eols(int eol_mode);

    /* Undo and redo can bring back line endings a conversion replaced, so the ending is read from the text again.
     * The claim is true when no recount is queued yet */
    bool claim_eol_recount();
    void recount_eols();

    /* Tabs or spaces by what most indented lines start with, and the width by the most common step between the
     * indents of neighboring lines, from the first indented lines. Null when no line is indented */
    std::optional<indentation> detect_indentation() const;
    void set_indentation(const indentation& indent);

    /* Rewrites every line's indentation in tabs or in spaces, as one undo step */
    void convert_indentation(bool to_tabs);

    /* Binds a document that was never saved to `path`, which the first save creates */
    void set_new_path(const QString& path);

    /* Compares the file on disk with the record of the last load or save */
    disk_state check_disk() const;

    /* Takes the file on disk as it is now as the record, so a change made elsewhere stops counting */
    void accept_disk_state();
    void set_deleted(bool is_deleted);

    view_state current_view_state() const;
    void restore_view_state(const view_state& state);

    bool is_modified() const;
    bool is_deleted() const { return m_is_deleted; }

    /* A file with no write bit, which opens editable and saves to a new file */
    bool is_read_only() const { return m_is_read_only; }

    /* Reads the write bit again. True when it changed */
    bool refresh_read_only();

    /* True once, at the first edit of a read-only file, so its banner shows a single time */
    bool claim_read_only_note();
    bool is_untitled() const { return m_file.path.isEmpty(); }
    bool is_pristine() const;
    int untitled_number() const { return m_untitled_number; }
    int changed_line_count() const;
    QString display_name() const;
    QString first_line_name() const;

    const std::optional<banner_message>& banner() const { return m_banner; }
    void set_banner(const std::optional<banner_message>& message) { m_banner = message; }

    ScintillaEditBase* view() const { return m_view; }
    const file_record& file() const { return m_file; }
    const language* lang() const { return m_language; }

private:
    void configure_view();
    void replace_text(const QByteArray& utf8);
    void remember_disk_state();
    void detect_language();
    QByteArray first_line() const;
    QByteArray text() const;

    ScintillaEditBase* m_view;
    file_record m_file;
    bool m_is_eol_recount_queued = false;
    const language* m_language = nullptr;
    edit_mode m_mode = edit_mode::TEXT;
    bool m_is_mode_chosen = false;
    int m_untitled_number;
    const sheen_settings& m_settings;
    bool m_is_deleted = false;
    bool m_is_read_only = false;
    bool m_is_read_only_noted = false;
    std::optional<banner_message> m_banner;
    QByteArray m_held_bytes;
    QWidget* m_placeholder = nullptr;
};

#endif
