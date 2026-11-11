#include "settings.h"
#include "encoding.h"

#include <QFile>
#include <QFileInfo>

struct setting_key {
    const char* section;
    const char* key;
    bool (*apply)(const QString& value, sheen_settings* settings);
};

struct line_ending_name {
    const char* name;
    int mode;
};

static constexpr float MIN_FONT_SIZE = 8;
static constexpr float MAX_FONT_SIZE = 48;
static constexpr int MAX_TAB_WIDTH = 16;
static constexpr int MAX_BLINK_MS = 5000;
static constexpr int MAX_COLUMN = 1000;
static constexpr int MAX_LARGE_FILE_MB = 4096;

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

static const setting_key KEYS[] = {
    { "editor", "font_size",
      [](const QString& value, sheen_settings* out) {
          return read_float(value, MIN_FONT_SIZE, MAX_FONT_SIZE, &out->font_size);
      } },
    { "editor", "tab_width",
      [](const QString& value, sheen_settings* out) { return read_int(value, 1, MAX_TAB_WIDTH, &out->tab_width); } },
    { "editor", "insert_spaces",
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->insert_spaces); } },
    { "editor", "detect_indentation",
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->detect_indentation); } },
    { "editor", "caret_blink_ms",
      [](const QString& value, sheen_settings* out) {
          return read_int(value, 0, MAX_BLINK_MS, &out->caret_blink_ms);
      } },
    { "code", "line_numbers",
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->code.line_numbers); } },
    { "code", "indent_guides",
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->code.indent_guides); } },
    { "code", "edge_column",
      [](const QString& value, sheen_settings* out) {
          return read_int(value, 0, MAX_COLUMN, &out->code.edge_column);
      } },
    { "code", "word_wrap",
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->code.word_wrap); } },
    { "text", "line_numbers",
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->text.line_numbers); } },
    { "text", "word_wrap",
      [](const QString& value, sheen_settings* out) { return read_bool(value, &out->text.word_wrap); } },
    { "text", "wrap_column",
      [](const QString& value, sheen_settings* out) {
          return read_int(value, 0, MAX_COLUMN, &out->text.wrap_column);
      } },
    { "files", "encoding",
      [](const QString& value, sheen_settings* out) { return read_encoding(value, &out->encoding); } },
    { "files", "line_ending",
      [](const QString& value, sheen_settings* out) { return read_line_ending(value, &out->line_ending); } },
    { "files", "large_file_mb",
      [](const QString& value, sheen_settings* out) {
          return read_int(value, 1, MAX_LARGE_FILE_MB, &out->large_file_mb);
      } },
    { "window", "menu_bar",
      [](const QString& value, sheen_settings* out) { return read_menu_bar(value, &out->has_menu_bar); } },
};

static const setting_key* find_key(const QString& section, const QString& key) {
    for (const setting_key& entry : KEYS) {
        if (section == QLatin1String(entry.section) && key == QLatin1String(entry.key)) {
            return &entry;
        }
    }

    return nullptr;
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
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) {
            continue;
        }

        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
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
