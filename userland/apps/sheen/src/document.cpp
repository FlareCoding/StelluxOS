#include "document.h"
#include "kiln.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <ScintillaEditBase.h>

#include <cerrno>
#include <sys/stat.h>

struct eol_counts {
    qsizetype crlf = 0;
    qsizetype cr = 0;
    qsizetype lf = 0;
};

struct key_binding {
    int key;
    int modifiers;
    int message;
};

// Enough of the first line to read the program a #! line names, or to name an untitled document
static constexpr qsizetype FIRST_LINE_PROBE_BYTES = 256;
static constexpr int MAX_NAME_CHARS = 32;
static constexpr qsizetype MAX_UTF8_BYTES = 4;
static constexpr int KEY_MODIFIER_SHIFT = 16;
static constexpr mode_t PERMISSION_BITS = 07777;
static constexpr QChar ELLIPSIS(0x2026);

static constexpr int CHANGE_HISTORY_SHOWN = SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS;
static constexpr int CHANGED_LINE_MARKERS =
    (1 << SC_MARKNUM_HISTORY_MODIFIED) | (1 << SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED);

static constexpr int SHIFT = SCMOD_SHIFT;
static constexpr int CTRL = SCMOD_CTRL;
static constexpr int ALT_SHIFT = SCMOD_ALT | SCMOD_SHIFT;
static constexpr int CTRL_SHIFT = SCMOD_CTRL | SCMOD_SHIFT;

// Scintilla's own keys that stay: caret movement, selection, deletion, the clipboard and the keys that type. Every
// other key belongs to Sheen's action table, so no default acts behind it unlisted
static const key_binding KEPT_KEYS[] = {
    { SCK_DOWN, SCMOD_NORM, SCI_LINEDOWN },
    { SCK_DOWN, SHIFT, SCI_LINEDOWNEXTEND },
    { SCK_DOWN, CTRL, SCI_LINESCROLLDOWN },
    { SCK_DOWN, ALT_SHIFT, SCI_LINEDOWNRECTEXTEND },
    { SCK_UP, SCMOD_NORM, SCI_LINEUP },
    { SCK_UP, SHIFT, SCI_LINEUPEXTEND },
    { SCK_UP, CTRL, SCI_LINESCROLLUP },
    { SCK_UP, ALT_SHIFT, SCI_LINEUPRECTEXTEND },
    { SCK_LEFT, SCMOD_NORM, SCI_CHARLEFT },
    { SCK_LEFT, SHIFT, SCI_CHARLEFTEXTEND },
    { SCK_LEFT, CTRL, SCI_WORDLEFT },
    { SCK_LEFT, CTRL_SHIFT, SCI_WORDLEFTEXTEND },
    { SCK_LEFT, ALT_SHIFT, SCI_CHARLEFTRECTEXTEND },
    { SCK_RIGHT, SCMOD_NORM, SCI_CHARRIGHT },
    { SCK_RIGHT, SHIFT, SCI_CHARRIGHTEXTEND },
    { SCK_RIGHT, CTRL, SCI_WORDRIGHT },
    { SCK_RIGHT, CTRL_SHIFT, SCI_WORDRIGHTEXTEND },
    { SCK_RIGHT, ALT_SHIFT, SCI_CHARRIGHTRECTEXTEND },
    { SCK_HOME, SCMOD_NORM, SCI_VCHOME },
    { SCK_HOME, SHIFT, SCI_VCHOMEEXTEND },
    { SCK_HOME, CTRL, SCI_DOCUMENTSTART },
    { SCK_HOME, CTRL_SHIFT, SCI_DOCUMENTSTARTEXTEND },
    { SCK_HOME, ALT_SHIFT, SCI_VCHOMERECTEXTEND },
    { SCK_END, SCMOD_NORM, SCI_LINEEND },
    { SCK_END, SHIFT, SCI_LINEENDEXTEND },
    { SCK_END, CTRL, SCI_DOCUMENTEND },
    { SCK_END, CTRL_SHIFT, SCI_DOCUMENTENDEXTEND },
    { SCK_END, ALT_SHIFT, SCI_LINEENDRECTEXTEND },
    { SCK_PRIOR, SCMOD_NORM, SCI_PAGEUP },
    { SCK_PRIOR, SHIFT, SCI_PAGEUPEXTEND },
    { SCK_PRIOR, ALT_SHIFT, SCI_PAGEUPRECTEXTEND },
    { SCK_NEXT, SCMOD_NORM, SCI_PAGEDOWN },
    { SCK_NEXT, SHIFT, SCI_PAGEDOWNEXTEND },
    { SCK_NEXT, ALT_SHIFT, SCI_PAGEDOWNRECTEXTEND },
    { SCK_DELETE, SCMOD_NORM, SCI_CLEAR },
    { SCK_DELETE, SHIFT, SCI_CUTALLOWLINE },
    { SCK_DELETE, CTRL, SCI_DELWORDRIGHT },
    { SCK_DELETE, CTRL_SHIFT, SCI_DELLINERIGHT },
    { SCK_INSERT, SCMOD_NORM, SCI_EDITTOGGLEOVERTYPE },
    { SCK_INSERT, SHIFT, SCI_PASTE },
    { SCK_INSERT, CTRL, SCI_COPYALLOWLINE },
    { SCK_BACK, SCMOD_NORM, SCI_DELETEBACK },
    { SCK_BACK, SHIFT, SCI_DELETEBACK },
    { SCK_BACK, CTRL, SCI_DELWORDLEFT },
    { SCK_BACK, CTRL_SHIFT, SCI_DELLINELEFT },
    { 'X', CTRL, SCI_CUTALLOWLINE },
    { 'C', CTRL, SCI_COPYALLOWLINE },
    { 'V', CTRL, SCI_PASTE },
    { SCK_TAB, SCMOD_NORM, SCI_TAB },
    { SCK_TAB, SHIFT, SCI_BACKTAB },
    { SCK_RETURN, SCMOD_NORM, SCI_NEWLINE },
    { SCK_RETURN, SHIFT, SCI_NEWLINE },
};

static eol_counts count_eols(QByteArrayView text) {
    eol_counts counts;
    for (qsizetype i = 0; i < text.size(); i++) {
        if (text[i] == '\n') {
            counts.lf++;
        } else if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            counts.crlf++;
            i++;
        } else if (text[i] == '\r') {
            counts.cr++;
        }
    }

    return counts;
}

// The ending most lines use, LF on a tie or when the text has a single line
static int majority_eol_mode(const eol_counts& counts) {
    if (counts.crlf > counts.lf && counts.crlf >= counts.cr) {
        return SC_EOL_CRLF;
    }

    return counts.cr > counts.lf && counts.cr > counts.crlf ? SC_EOL_CR : SC_EOL_LF;
}

static bool has_mixed_eols(const eol_counts& counts) {
    return (counts.crlf > 0) + (counts.cr > 0) + (counts.lf > 0) > 1;
}

document::document(QWidget* parent, int untitled_number)
    : m_view(new ScintillaEditBase(parent)), m_untitled_number(untitled_number) {
    configure_view();
}

document::~document() {
    delete m_view;
}

bool document::load(const QString& path, const QByteArray& bytes, const QByteArray& encoding) {
    decoded_text text;
    if (encoding.isEmpty()) {
        text = decode_detected(bytes);
    } else if (!decode_as(bytes, encoding, &text)) {
        return false;
    }

    m_file.path = path;
    m_file.encoding = text.encoding;
    replace_text(text.utf8);
    detect_language();
    return true;
}

bool document::reload_as(const QByteArray& encoding, QString* error) {
    QFile file(m_file.path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }

    decoded_text text;
    if (!decode_as(file.readAll(), encoding, &text)) {
        *error = QStringLiteral(" is not valid %1").arg(encoding_label(encoding));
        return false;
    }

    m_file.encoding = text.encoding;
    replace_text(text.utf8);
    return true;
}

bool document::reload_from_disk(QString* error) {
    QFile file(m_file.path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }

    QByteArray bytes = file.readAll();
    decoded_text text;
    if (!decode_as(bytes, m_file.encoding.name, &text)) {
        text = decode_detected(bytes);
    }

    sptr_t caret = m_view->send(SCI_GETCURRENTPOS);
    sptr_t line = m_view->send(SCI_LINEFROMPOSITION, caret);
    sptr_t column = m_view->send(SCI_GETCOLUMN, caret);
    sptr_t first_line = m_view->send(SCI_GETFIRSTVISIBLELINE);

    // One undo step, so Ctrl Z brings back the text from before the reload
    m_view->send(SCI_BEGINUNDOACTION);
    m_view->send(SCI_TARGETWHOLEDOCUMENT);
    m_view->send(SCI_REPLACETARGET, text.utf8.size(), reinterpret_cast<sptr_t>(text.utf8.constData()));
    m_view->send(SCI_ENDUNDOACTION);
    m_view->send(SCI_SETSAVEPOINT);

    sptr_t last_line = m_view->send(SCI_GETLINECOUNT) - 1;
    m_view->send(SCI_GOTOPOS, m_view->send(SCI_FINDCOLUMN, qMin(line, last_line), column));
    m_view->send(SCI_SETFIRSTVISIBLELINE, first_line);

    eol_counts counts = count_eols(text.utf8);
    m_file.encoding = text.encoding;
    set_eol_mode(majority_eol_mode(counts));
    m_file.has_mixed_eols = has_mixed_eols(counts);

    m_is_deleted = false;
    remember_disk_state();
    return true;
}

bool document::save(const QString& path, QString* error, unencodable_char* unencodable) {
    QByteArray utf8 = text();
    encoded_text encoded = encode_text(utf8, m_file.encoding);
    if (encoded.unencodable_at >= 0) {
        unencodable->line = m_view->send(SCI_LINEFROMPOSITION, encoded.unencodable_at) + 1;
        unencodable->code = QString::fromUtf8(utf8.mid(encoded.unencodable_at, MAX_UTF8_BYTES)).toUcs4().value(0);
        return false;
    }

    // QFileInfo also reports what this user may do, which QSaveFile writes back as owner bits, so the file's own
    // mode is read first and restored after the rename
    QByteArray native_path = QFile::encodeName(path);
    struct stat original = {};
    bool has_mode = ::stat(native_path.constData(), &original) == 0;

    // A folder that takes no new files, such as devfs, is named in plain words instead of by its errno
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        bool is_refused = errno == ENOSYS || errno == EROFS;
        *error = is_refused ? QStringLiteral("%1 takes no new files").arg(QFileInfo(path).absolutePath())
                            : file.errorString();
        return false;
    }

    if (file.write(encoded.bytes) != encoded.bytes.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }

    if (has_mode) {
        (void)::chmod(native_path.constData(), original.st_mode & PERMISSION_BITS);
    }

    m_view->send(SCI_SETSAVEPOINT);
    bool is_renamed = path != m_file.path;
    m_file.path = path;
    m_is_deleted = false;
    remember_disk_state();
    if (is_renamed) {
        detect_language();
    }

    return true;
}

void document::set_encoding(const file_encoding& encoding) {
    m_file.encoding = encoding;
}

void document::set_language(const language* lang) {
    m_language = lang;
    apply_language(m_view, m_language);
}

void document::set_eol_mode(int eol_mode) {
    m_file.eol_mode = eol_mode;
    m_view->send(SCI_SETEOLMODE, eol_mode);
}

void document::convert_eols(int eol_mode) {
    m_view->send(SCI_CONVERTEOLS, eol_mode);
    set_eol_mode(eol_mode);
    m_file.has_mixed_eols = false;
}

bool document::claim_eol_recount() {
    bool is_first = !m_is_eol_recount_queued;
    m_is_eol_recount_queued = true;
    return is_first;
}

void document::recount_eols() {
    m_is_eol_recount_queued = false;
    eol_counts counts = count_eols(text());
    set_eol_mode(majority_eol_mode(counts));
    m_file.has_mixed_eols = has_mixed_eols(counts);
}

void document::set_new_path(const QString& path) {
    m_file.path = path;
    detect_language();
}

disk_state document::check_disk() const {
    if (m_file.path.isEmpty() || m_file.size < 0) {
        return disk_state::SAME;
    }

    QFileInfo info(m_file.path);
    if (!info.isFile()) {
        return disk_state::DELETED;
    }

    bool is_changed = info.size() != m_file.size || info.lastModified() != m_file.modified;
    return is_changed ? disk_state::CHANGED : disk_state::SAME;
}

void document::accept_disk_state() {
    remember_disk_state();
}

void document::set_deleted(bool is_deleted) {
    m_is_deleted = is_deleted;
}

// A file deleted on disk counts as unsaved, since its text now lives only here
bool document::is_modified() const {
    return m_is_deleted || m_view->send(SCI_GETMODIFY) != 0;
}

// An untouched untitled document, which a file opened next takes the place of
bool document::is_pristine() const {
    return m_file.path.isEmpty() && !is_modified() && m_view->send(SCI_GETLENGTH) == 0;
}

int document::changed_line_count() const {
    int count = 0;
    for (sptr_t line = m_view->send(SCI_MARKERNEXT, 0, CHANGED_LINE_MARKERS); line >= 0;
         line = m_view->send(SCI_MARKERNEXT, line + 1, CHANGED_LINE_MARKERS)) {
        count++;
    }

    return count;
}

QString document::display_name() const {
    if (!m_file.path.isEmpty()) {
        return QFileInfo(m_file.path).fileName();
    }

    QString first_line = first_line_name();
    if (!first_line.isEmpty()) {
        return first_line;
    }

    return m_untitled_number == 1 ? QStringLiteral("Untitled") : QStringLiteral("Untitled %1").arg(m_untitled_number);
}

// The first line without heading marks, cut to a tab's length
QString document::first_line_name() const {
    QString line = QString::fromUtf8(first_line()).trimmed();
    while (line.startsWith(QLatin1Char('#'))) {
        line.remove(0, 1);
    }

    line = line.trimmed();
    return line.size() > MAX_NAME_CHARS ? line.left(MAX_NAME_CHARS).trimmed() + ELLIPSIS : line;
}

void document::configure_view() {
    m_view->send(SCI_SETCODEPAGE, SC_CP_UTF8);
    m_view->send(SCI_SETEOLMODE, m_file.eol_mode);
    m_view->send(SCI_CLEARALLCMDKEYS);
    for (const key_binding& binding : KEPT_KEYS) {
        m_view->send(SCI_ASSIGNCMDKEY, binding.key | (binding.modifiers << KEY_MODIFIER_SHIFT), binding.message);
    }

    m_view->send(SCI_SETMULTIPLESELECTION, 1);
    m_view->send(SCI_SETADDITIONALSELECTIONTYPING, 1);
    m_view->send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
    apply_text_area_look(m_view);
    m_view->send(SCI_SETCHANGEHISTORY, CHANGE_HISTORY_SHOWN);
}

void document::replace_text(const QByteArray& utf8) {
    m_view->send(SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED);
    m_view->send(SCI_SETUNDOCOLLECTION, 0);
    m_view->send(SCI_CLEARALL);
    m_view->send(SCI_APPENDTEXT, utf8.size(), reinterpret_cast<sptr_t>(utf8.constData()));
    m_view->send(SCI_SETUNDOCOLLECTION, 1);

    m_view->send(SCI_EMPTYUNDOBUFFER);
    m_view->send(SCI_SETSAVEPOINT);

    // Change history starts only on an empty undo buffer
    m_view->send(SCI_SETCHANGEHISTORY, CHANGE_HISTORY_SHOWN);
    m_view->send(SCI_GOTOPOS, 0);

    eol_counts counts = count_eols(utf8);
    set_eol_mode(majority_eol_mode(counts));
    m_file.has_mixed_eols = has_mixed_eols(counts);
    m_is_deleted = false;
    remember_disk_state();
    fit_line_numbers(m_view);
}

void document::remember_disk_state() {
    QFileInfo info(m_file.path);
    m_file.size = info.isFile() ? info.size() : -1;
    m_file.modified = info.lastModified();
}

void document::detect_language() {
    set_language(find_language(QFileInfo(m_file.path).fileName(), first_line()));
}

// Copied rather than pointed at, since a pointer into the text moves Scintilla's gap, which change notifications
// must not do
QByteArray document::first_line() const {
    Sci_TextRangeFull range;
    range.chrg.cpMin = 0;
    range.chrg.cpMax = qMin<sptr_t>(m_view->send(SCI_GETLINEENDPOSITION, 0), FIRST_LINE_PROBE_BYTES);
    QByteArray line(range.chrg.cpMax + 1, '\0');
    range.lpstrText = line.data();
    m_view->send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&range));

    line.resize(range.chrg.cpMax);
    return line;
}

// The pointer stays valid while the text is unchanged, which holds for as long as the caller uses it
QByteArray document::text() const {
    sptr_t length = m_view->send(SCI_GETLENGTH);
    const char* characters = reinterpret_cast<const char*>(m_view->send(SCI_GETCHARACTERPOINTER));
    return QByteArray::fromRawData(characters, length);
}
