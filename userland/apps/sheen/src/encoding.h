#ifndef SHEEN_ENCODING_H
#define SHEEN_ENCODING_H

/* Text encodings of files on disk. A document always holds UTF-8, decoded once at open and encoded once at save,
 * Unicode through Qt's converters and legacy code pages through Core5Compat's `QTextCodec`. */
#include <QByteArray>
#include <QList>
#include <QString>

/* An encoding by the name Qt knows it under, such as UTF-8, UTF-16LE or windows-1252 */
struct file_encoding {
    QByteArray name;
    bool has_bom = false;
};

struct decoded_text {
    QByteArray utf8;
    file_encoding encoding;
};

struct encoded_text {
    QByteArray bytes;
    qsizetype unencodable_at = -1; // where the first character the encoding cannot hold starts in the UTF-8 text
};

/* Reads a byte order mark first, then valid UTF-8, then Windows-1252 when every byte is defined there, and falls
 * back to Latin-1, which maps every byte. Both single byte encodings give back the same bytes on save. */
decoded_text decode_detected(const QByteArray& bytes);

/* Decodes `bytes` as `name`, skipping its byte order mark. Returns false when they are not valid in it */
bool decode_as(const QByteArray& bytes, const QByteArray& name, decoded_text* text);

encoded_text encode_text(const QByteArray& utf8, const file_encoding& encoding);

/* NUL bytes near the start mark a file that is not text in any single or multi byte encoding Sheen detects */
bool looks_binary(const QByteArray& bytes);

/* UTF-16 LE or BE when the NULs of a file without a byte order mark fall on every other byte, as ASCII text in
 * UTF-16 has them, empty otherwise */
QByteArray guess_utf16(const QByteArray& bytes);

/* The encodings Sheen can read and write here, Unicode first */
QList<QByteArray> available_encodings();

QString encoding_label(const QByteArray& name);
QString encoding_label(const file_encoding& encoding);

#endif
