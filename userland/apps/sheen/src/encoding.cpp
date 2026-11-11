#include "encoding.h"

#include <QStringDecoder>
#include <QStringEncoder>
#include <QTextCodec>

#include <optional>

struct byte_order_mark {
    QByteArrayView bytes;
    const char* encoding;
};

struct encoding_label_entry {
    const char* name;
    const char* label;
};

static const QByteArray UTF_8 = QByteArrayLiteral("UTF-8");
static const QByteArray WINDOWS_1252 = QByteArrayLiteral("windows-1252");
static const QByteArray LATIN_1 = QByteArrayLiteral("ISO-8859-1");

static constexpr qsizetype BINARY_PROBE_BYTES = 64 * 1024;
static constexpr qsizetype VALIDATION_CHUNK_BYTES = 1024 * 1024;
static constexpr uchar UTF8_CONTINUATION_MASK = 0xC0;
static constexpr uchar UTF8_CONTINUATION_BITS = 0x80;

// A second mark after the first one is text, which the converters keep instead of skipping
static constexpr QStringConverter::Flags CONVERTER_FLAGS =
    QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom;

// UTF-32 LE's mark begins with UTF-16 LE's, so the longer marks come first
static const byte_order_mark BYTE_ORDER_MARKS[] = {
    { QByteArrayView("\xFF\xFE\x00\x00", 4), "UTF-32LE" },
    { QByteArrayView("\x00\x00\xFE\xFF", 4), "UTF-32BE" },
    { QByteArrayView("\xEF\xBB\xBF", 3), "UTF-8" },
    { QByteArrayView("\xFF\xFE", 2), "UTF-16LE" },
    { QByteArrayView("\xFE\xFF", 2), "UTF-16BE" },
};

// The encodings Qt's string converters handle without ICU
static const char* const CONVERTER_ENCODINGS[] = {
    "UTF-8", "UTF-16LE", "UTF-16BE", "UTF-32LE", "UTF-32BE", "ISO-8859-1",
};

static const char* const LEGACY_ENCODINGS[] = {
    "windows-1250", "windows-1251", "windows-1252", "windows-1253", "windows-1254", "windows-1255",
    "windows-1256", "windows-1257", "windows-1258", "ISO-8859-2",   "ISO-8859-5",   "ISO-8859-7",
    "ISO-8859-9",   "ISO-8859-15",  "KOI8-R",       "KOI8-U",       "Shift_JIS",    "EUC-JP",
    "EUC-KR",       "GB18030",      "Big5",
};

static const encoding_label_entry LABELS[] = {
    { "UTF-16LE", "UTF-16 LE" },
    { "UTF-16BE", "UTF-16 BE" },
    { "UTF-32LE", "UTF-32 LE" },
    { "UTF-32BE", "UTF-32 BE" },
    { "ISO-8859-1", "Latin-1" },
};

static QByteArrayView byte_order_mark_of(const QByteArray& name) {
    for (const byte_order_mark& mark : BYTE_ORDER_MARKS) {
        if (name == mark.encoding) {
            return mark.bytes;
        }
    }

    return QByteArrayView();
}

// Chunks end before a lead byte, so no sequence spans two of them and each validates on its own
static bool is_valid_utf8(QByteArrayView bytes) {
    qsizetype start = 0;
    while (start < bytes.size()) {
        qsizetype end = qMin(start + VALIDATION_CHUNK_BYTES, bytes.size());
        while (end < bytes.size() && end > start
               && (static_cast<uchar>(bytes[end]) & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_BITS) {
            end--;
        }

        if (end == start) {
            end = qMin(start + VALIDATION_CHUNK_BYTES, bytes.size());
        }

        QStringDecoder decoder(QStringConverter::Utf8, CONVERTER_FLAGS);
        QString chunk = decoder.decode(bytes.sliced(start, end - start));
        if (decoder.hasError()) {
            return false;
        }

        start = end;
    }

    return true;
}

// A legacy codec counts as valid only when its text encodes back to the same bytes, so a save never changes them
static bool decode_to_utf8(QByteArrayView bytes, const QByteArray& name, QByteArray* utf8) {
    if (name == UTF_8) {
        if (!is_valid_utf8(bytes)) {
            return false;
        }

        *utf8 = bytes.toByteArray();
        return true;
    }

    if (std::optional<QStringConverter::Encoding> encoding = QStringConverter::encodingForName(name)) {
        QStringDecoder decoder(*encoding, CONVERTER_FLAGS);
        QString text = decoder.decode(bytes);
        if (decoder.hasError()) {
            return false;
        }

        *utf8 = text.toUtf8();
        return true;
    }

    QTextCodec* codec = QTextCodec::codecForName(name);
    if (!codec) {
        return false;
    }

    QString text = codec->toUnicode(bytes.data(), static_cast<int>(bytes.size()));
    if (codec->fromUnicode(text) != bytes) {
        return false;
    }

    *utf8 = text.toUtf8();
    return true;
}

static bool encode_from_utf8(QByteArrayView utf8, const QByteArray& name, QByteArray* bytes) {
    if (name == UTF_8) {
        *bytes = utf8.toByteArray();
        return true;
    }

    QString text = QString::fromUtf8(utf8);
    if (std::optional<QStringConverter::Encoding> encoding = QStringConverter::encodingForName(name)) {
        // A stateless encoder writes ? for a character it cannot hold without counting it as an error
        QStringEncoder encoder(*encoding);
        *bytes = encoder.encode(text);
        return !encoder.hasError();
    }

    QTextCodec* codec = QTextCodec::codecForName(name);
    if (!codec || !codec->canEncode(text)) {
        return false;
    }

    *bytes = codec->fromUnicode(text);
    return true;
}

// Only a line known to hold a character the encoding cannot is walked a character at a time
static qsizetype first_unencodable_char(QByteArrayView line, const QByteArray& name) {
    qsizetype at = 0;
    while (at < line.size()) {
        qsizetype next = at + 1;
        while (next < line.size() && (static_cast<uchar>(line[next]) & 0xC0) == 0x80) {
            next++;
        }

        QByteArray encoded;
        if (!encode_from_utf8(line.sliced(at, next - at), name, &encoded)) {
            return at;
        }

        at = next;
    }

    return 0;
}

static qsizetype first_unencodable_at(QByteArrayView utf8, const QByteArray& name) {
    qsizetype start = 0;
    while (start < utf8.size()) {
        qsizetype end = start;
        while (end < utf8.size() && utf8[end] != '\n' && utf8[end] != '\r') {
            end++;
        }

        QByteArray line;
        if (!encode_from_utf8(utf8.sliced(start, end - start), name, &line)) {
            return start + first_unencodable_char(utf8.sliced(start, end - start), name);
        }

        start = end + 1;
    }

    return 0;
}

decoded_text decode_detected(const QByteArray& bytes) {
    decoded_text text;
    for (const byte_order_mark& mark : BYTE_ORDER_MARKS) {
        if (bytes.startsWith(mark.bytes) && decode_as(bytes, mark.encoding, &text)) {
            return text;
        }
    }

    if (decode_as(bytes, UTF_8, &text) || decode_as(bytes, WINDOWS_1252, &text)) {
        return text;
    }

    decode_as(bytes, LATIN_1, &text);
    return text;
}

bool decode_as(const QByteArray& bytes, const QByteArray& name, decoded_text* text) {
    QByteArrayView content = bytes;
    QByteArrayView mark = byte_order_mark_of(name);
    bool has_bom = !mark.isEmpty() && content.startsWith(mark);
    if (has_bom) {
        content = content.sliced(mark.size());
    }

    QByteArray utf8;
    if (!decode_to_utf8(content, name, &utf8)) {
        return false;
    }

    text->utf8 = std::move(utf8);
    text->encoding = { name, has_bom };
    return true;
}

encoded_text encode_text(const QByteArray& utf8, const file_encoding& encoding) {
    encoded_text encoded;
    QByteArray body;
    if (!encode_from_utf8(utf8, encoding.name, &body)) {
        encoded.unencodable_at = first_unencodable_at(utf8, encoding.name);
        return encoded;
    }

    encoded.bytes = encoding.has_bom ? byte_order_mark_of(encoding.name).toByteArray() + body : body;
    return encoded;
}

bool looks_binary(const QByteArray& bytes) {
    for (const byte_order_mark& mark : BYTE_ORDER_MARKS) {
        if (bytes.startsWith(mark.bytes)) {
            return false;
        }
    }

    return QByteArrayView(bytes).first(qMin(bytes.size(), BINARY_PROBE_BYTES)).contains('\0');
}

QList<QByteArray> available_encodings() {
    QList<QByteArray> names;
    for (const char* name : CONVERTER_ENCODINGS) {
        names.append(name);
    }

    for (const char* name : LEGACY_ENCODINGS) {
        if (QTextCodec::codecForName(name)) {
            names.append(name);
        }
    }

    return names;
}

QString encoding_label(const QByteArray& name) {
    for (const encoding_label_entry& entry : LABELS) {
        if (name == entry.name) {
            return QString::fromLatin1(entry.label);
        }
    }

    if (name.startsWith("windows-")) {
        return QStringLiteral("Windows-") + QString::fromLatin1(name.mid(qstrlen("windows-")));
    }

    return QString::fromLatin1(name);
}

QString encoding_label(const file_encoding& encoding) {
    QString label = encoding_label(encoding.name);
    return encoding.has_bom && encoding.name == UTF_8 ? label + QStringLiteral(" with BOM") : label;
}
