#ifndef SHEEN_DOCUMENT_H
#define SHEEN_DOCUMENT_H

/* One open file: the Scintilla view that edits it in UTF-8, its language, and the record of the file on disk with
 * the encoding and line ending it keeps. A document without a path has never been saved. */
#include "banner.h"
#include "encoding.h"

#include <QDateTime>
#include <QString>

#include <Scintilla.h>

#include <optional>

class ScintillaEditBase;
class QWidget;
struct language;

struct file_record {
    QString path;
    file_encoding encoding = { QByteArrayLiteral("UTF-8"), false };
    int eol_mode = SC_EOL_LF;    // the line ending new lines take
    bool has_mixed_eols = false;
    qint64 size = -1;            // at the last load or save, to notice a change made elsewhere
    QDateTime modified;
};

enum class disk_state { SAME, CHANGED, DELETED };

/* The first character an encoding cannot hold, as a refused save names it */
struct unencodable_char {
    sptr_t line = 0; // counted from one, zero when every character was held
    char32_t code = 0;
};

class document {
public:
    /* `untitled_number` names the document until it has a path or a first line, "Untitled 2" for 2 */
    document(QWidget* parent, int untitled_number);
    ~document();

    document(const document&) = delete;
    document& operator=(const document&) = delete;

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

    void set_encoding(const file_encoding& encoding);
    void set_language(const language* lang);
    void set_eol_mode(int eol_mode);
    void convert_eols(int eol_mode);

    /* Undo and redo can bring back line endings a conversion replaced, so the ending is read from the text again.
     * The claim is true when no recount is queued yet */
    bool claim_eol_recount();
    void recount_eols();

    /* Binds a document that was never saved to `path`, which the first save creates */
    void set_new_path(const QString& path);

    /* Compares the file on disk with the record of the last load or save */
    disk_state check_disk() const;

    /* Takes the file on disk as it is now as the record, so a change made elsewhere stops counting */
    void accept_disk_state();
    void set_deleted(bool is_deleted);

    bool is_modified() const;
    bool is_deleted() const { return m_is_deleted; }
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
    int m_untitled_number;
    bool m_is_deleted = false;
    std::optional<banner_message> m_banner;
};

#endif
