#include "settings.h"
#include "encoding.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

struct setting_key {
    setting_field field;
    bool (*apply)(const QString& value, sheen_settings* settings);
    QString (*value)(const sheen_settings& settings);
};

struct line_ending_name {
    const char* name;
    int mode;
};

static constexpr float MIN_FONT_SIZE = 8;
static constexpr float MAX_FONT_SIZE = 48;
static constexpr float FONT_SIZE_STEP = 0.5f;
static constexpr int MAX_TAB_WIDTH = 16;
static constexpr int MAX_BLINK_MS = 5000;
static constexpr int BLINK_STEP_MS = 100;
static constexpr int MAX_COLUMN = 1000;
static constexpr int MAX_LARGE_FILE_MB = 4096;
static constexpr int LARGE_FILE_STEP_MB = 8;

static constexpr line_ending_name LINE_ENDINGS[] = {
    { "lf", SC_EOL_LF },
    { "crlf", SC_EOL_CRLF },
    { "cr", SC_EOL_CR },
};

static bool read_bool(const QString& value, bool* setting) {
    if (value != QLatin1String("true") && value != QLatin1String("false")) {
        return false;
    }

    *setting = value == QLatin1String("true");
    return true;
}

static bool read_int(const QString& value, int min, int max, int* setting) {
    bool is_number = false;
    int number = value.toInt(&is_number);
    if (!is_number || number < min || number > max) {
        return false;
    }

    *setting = number;
    return true;
}

static bool read_float(const QString& value, float min, float max, float* setting) {
    bool is_number = false;
    float number = value.toFloat(&is_number);
    if (!is_number || number < min || number > max) {
        return false;
    }

    *setting = number;
    return true;
}

static bool read_encoding(const QString& value, QByteArray* setting) {
    for (const QByteArray& name : available_encodings()) {
        if (value.compare(QString::fromLatin1(name), Qt::CaseInsensitive) == 0) {
            *setting = name;
            return true;
        }
    }

    return false;
}

static bool read_line_ending(const QString& value, int* setting) {
    for (const line_ending_name& ending : LINE_ENDINGS) {
        if (value == QLatin1String(ending.name)) {
            *setting = ending.mode;
            return true;
        }
    }

    return false;
}

static bool read_menu_bar(const QString& value, bool* has_menu_bar) {
    if (value != QLatin1String("button") && value != QLatin1String("bar")) {
        return false;
    }

    *has_menu_bar = value == QLatin1String("bar");
    return true;
}

static QString bool_text(bool value) {
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

static QString line_ending_text(int mode) {
    for (const line_ending_name& ending : LINE_ENDINGS) {
        if (ending.mode == mode) {
            return QString::fromLatin1(ending.name);
        }
    }

    return QString::fromLatin1(LINE_ENDINGS[0].name);
}

static constexpr setting_field toggle(const char* section, const char* key, const char* page, const char* group,
                                      const char* label, const char* note) {
    return { section, key, page, group, label, note, setting_control::TOGGLE, "", "", 0, 0, 0, "" };
}

static constexpr setting_field choice(const char* section, const char* key, const char* page, const char* group,
                                      const char* label, const char* note, const char* choices, const char* labels) {
    return { section, key, page, group, label, note, setting_control::CHOICE, choices, labels, 0, 0, 0, "" };
}

static constexpr setting_field number(const char* section, const char* key, const char* page, const char* group,
                                      const char* label, const char* note, double minimum, double maximum,
                                      double step, const char* unit) {
    return { section, key, page, group, label, note, setting_control::NUMBER, "", "", minimum, maximum, step, unit };
}

static constexpr setting_field encoding_list(const char* section, const char* key, const char* page,
                                             const char* group, const char* label, const char* note) {
    return { section, key, page, group, label, note, setting_control::ENCODING, "", "", 0, 0, 0, "" };
}

static const setting_key KEYS[] = {
    { number("editor", "font_size", "Editor", "Type", "Font size", "", MIN_FONT_SIZE, MAX_FONT_SIZE, FONT_SIZE_STEP,
             "px"),
      [](const QString& value, sheen_settings* out) {
          return read_float(value, MIN_FONT_SIZE, MAX_FONT_SIZE, &out->font_size);
      },
      [](const sheen_settings& in) { return QString::number(in.font_size); } },
    { toggle("editor", "detect_indentation", "Editor", "Indentation", "Detect from each file",
             "Tabs or spaces and their width, from the first 1,000 indented lines"),
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->detect_indentation); },
      [](const sheen_settings& in) { return bool_text(in.detect_indentation); } },
    { choice("editor", "insert_spaces", "Editor", "Indentation", "Indent new lines with", "", "true false",
             "Spaces Tabs"),
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->insert_spaces); },
      [](const sheen_settings& in) { return bool_text(in.insert_spaces); } },
    { choice("editor", "tab_width", "Editor", "Indentation", "Tab width", "", "2 4 8", "2 4 8"),
      [](const QString& value, sheen_settings* out) { return read_int(value, 1, MAX_TAB_WIDTH, &out->tab_width); },
      [](const sheen_settings& in) { return QString::number(in.tab_width); } },
    { number("editor", "caret_blink_ms", "Editor", "Caret", "Blink interval", "0 holds the caret steady", 0,
             MAX_BLINK_MS, BLINK_STEP_MS, "ms"),
      [](const QString& value, sheen_settings* out) {
          return read_int(value, 0, MAX_BLINK_MS, &out->caret_blink_ms);
      },
      [](const sheen_settings& in) { return QString::number(in.caret_blink_ms); } },
    { toggle("code", "line_numbers", "Code", "View", "Line numbers", ""),
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->code.line_numbers); },
      [](const sheen_settings& in) { return bool_text(in.code.line_numbers); } },
    { toggle("code", "indent_guides", "Code", "View", "Indent guides", ""),
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->code.indent_guides); },
      [](const sheen_settings& in) { return bool_text(in.code.indent_guides); } },
    { number("code", "edge_column", "Code", "View", "Column edge", "0 hides the edge", 0, MAX_COLUMN, 1, "columns"),
      [](const QString& value, sheen_settings* out) {
          return read_int(value, 0, MAX_COLUMN, &out->code.edge_column);
      },
      [](const sheen_settings& in) { return QString::number(in.code.edge_column); } },
    { toggle("code", "word_wrap", "Code", "View", "Word wrap", ""),
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->code.word_wrap); },
      [](const sheen_settings& in) { return bool_text(in.code.word_wrap); } },
    { toggle("text", "line_numbers", "Text", "View", "Line numbers", ""),
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->text.line_numbers); },
      [](const sheen_settings& in) { return bool_text(in.text.line_numbers); } },
    { toggle("text", "word_wrap", "Text", "View", "Word wrap", ""),
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->text.word_wrap); },
      [](const sheen_settings& in) { return bool_text(in.text.word_wrap); } },
    { number("text", "wrap_column", "Text", "View", "Reading width", "0 wraps at the window's edge", 0, MAX_COLUMN, 1,
             "columns"),
      [](const QString& value, sheen_settings* out) {
          return read_int(value, 0, MAX_COLUMN, &out->text.wrap_column);
      },
      [](const sheen_settings& in) { return QString::number(in.text.wrap_column); } },
    { encoding_list("files", "encoding", "Files", "New files", "Encoding", ""),
      [](const QString& value, sheen_settings* out) { return read_encoding(value, &out->encoding); },
      [](const sheen_settings& in) { return QString::fromLatin1(in.encoding); } },
    { choice("files", "line_ending", "Files", "New files", "Line endings", "", "lf crlf cr", "LF CRLF CR"),
      [](const QString& value, sheen_settings* out) { return read_line_ending(value, &out->line_ending); },
      [](const sheen_settings& in) { return line_ending_text(in.line_ending); } },
    { number("files", "large_file_mb", "Files", "Large files", "Large file mode from",
             "Plain and unwrapped, with no highlighting or folding", 1, MAX_LARGE_FILE_MB, LARGE_FILE_STEP_MB, "MB"),
      [](const QString& value, sheen_settings* out) {
          return read_int(value, 1, MAX_LARGE_FILE_MB, &out->large_file_mb);
      },
      [](const sheen_settings& in) { return QString::number(in.large_file_mb); } },
    { choice("window", "menu_bar", "Editor", "Window", "Menus", "Takes effect when Sheen starts", "button bar",
             "Button Bar"),
      [](const QString& value, sheen_settings* out) { return read_menu_bar(value, &out->has_menu_bar); },
      [](const sheen_settings& in) { return in.has_menu_bar ? QStringLiteral("bar") : QStringLiteral("button"); } },
};

static const setting_key* find_key(const QString& section, const QString& key) {
    for (const setting_key& entry : KEYS) {
        if (section == QLatin1String(entry.field.section) && key == QLatin1String(entry.field.key)) {
            return &entry;
        }
    }

    return nullptr;
}

static bool is_section_line(const QString& line) {
    return line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'));
}

static bool is_comment_line(const QString& line) {
    return line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'));
}

QStringList read_settings(const QString& path, sheen_settings* settings) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringList();
    }

    QStringList problems;
    QString name = QFileInfo(path).fileName();
    QString section;
    int number = 0;
    for (const QByteArray& raw : file.readAll().split('\n')) {
        number++;
        QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || is_comment_line(line)) {
            continue;
        }

        if (is_section_line(line)) {
            section = line.mid(1, line.size() - 2).trimmed();
            continue;
        }

        qsizetype equals = line.indexOf(QLatin1Char('='));
        QString key = line.left(equals).trimmed();
        const setting_key* entry = equals > 0 ? find_key(section, key) : nullptr;
        if (!entry) {
            problems.append(QStringLiteral("%1:%2: unknown key %3").arg(name).arg(number).arg(key));
            continue;
        }

        if (!entry->apply(line.mid(equals + 1).trimmed(), settings)) {
            problems.append(QStringLiteral("%1:%2: bad value for %3").arg(name).arg(number).arg(key));
        }
    }

    return problems;
}

QList<setting_field> setting_fields() {
    QList<setting_field> fields;
    for (const setting_key& entry : KEYS) {
        fields.append(entry.field);
    }

    return fields;
}

QString setting_value(const sheen_settings& settings, const QString& section, const QString& key) {
    const setting_key* entry = find_key(section, key);
    return entry ? entry->value(settings) : QString();
}

// A new sheen.conf lists every key at its default under its section, in the table's order
static QStringList default_lines() {
    sheen_settings defaults;
    QStringList lines;
    QString section;
    for (const setting_key& entry : KEYS) {
        if (section != QLatin1String(entry.field.section)) {
            if (!lines.isEmpty()) {
                lines.append(QString());
            }

            section = QString::fromLatin1(entry.field.section);
            lines.append(QStringLiteral("[%1]").arg(section));
        }

        lines.append(QStringLiteral("%1 = %2").arg(QString::fromLatin1(entry.field.key), entry.value(defaults)));
    }

    return lines;
}

// Keeps the key and its spacing around `=` and replaces only what follows
static QString with_value(const QString& line, const QString& value) {
    qsizetype after = line.indexOf(QLatin1Char('=')) + 1;
    while (after < line.size() && line.at(after) == QLatin1Char(' ')) {
        after++;
    }

    return line.left(after) + value;
}

// Sets `key` on its line in `section`, else adds it after the section's last key, else adds the section at the end
static void set_line(QStringList* lines, const QString& section, const QString& key, const QString& value) {
    QString current;
    qsizetype insert_at = -1;
    for (qsizetype index = 0; index < lines->size(); index++) {
        QString line = lines->at(index).trimmed();
        if (is_section_line(line)) {
            current = line.mid(1, line.size() - 2).trimmed();
            insert_at = current == section ? index + 1 : insert_at;
            continue;
        }

        qsizetype equals = line.indexOf(QLatin1Char('='));
        bool is_key = equals > 0 && !is_comment_line(line);
        if (current != section || !is_key) {
            continue;
        }

        if (line.left(equals).trimmed() == key) {
            (*lines)[index] = with_value(lines->at(index), value);
            return;
        }

        insert_at = index + 1;
    }

    QString entry = QStringLiteral("%1 = %2").arg(key, value);
    if (insert_at >= 0) {
        lines->insert(insert_at, entry);
        return;
    }

    if (!lines->isEmpty() && !lines->last().trimmed().isEmpty()) {
        lines->append(QString());
    }

    lines->append(QStringLiteral("[%1]").arg(section));
    lines->append(entry);
}

bool write_settings(const QString& path, const QHash<QString, QString>& changes, QString* error) {
    QStringList lines;
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        lines = QString::fromUtf8(existing.readAll()).split(QLatin1Char('\n'));
        if (!lines.isEmpty() && lines.last().isEmpty()) {
            lines.removeLast();
        }
    } else {
        lines = default_lines();
    }

    for (auto change = changes.begin(); change != changes.end(); ++change) {
        set_line(&lines, change.key().section(QLatin1Char('.'), 0, 0), change.key().section(QLatin1Char('.'), 1),
                 change.value());
    }

    QByteArray bytes = (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    bool is_written = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    if (!is_written) {
        *error = file.errorString();
    }

    return is_written;
}
