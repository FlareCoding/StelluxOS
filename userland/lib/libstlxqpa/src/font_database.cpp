#include "font_database.h"

#include <QDir>
#include <QFile>

static const char* FONT_DIR = "/etc/res/fonts";

void QStelluxFontDatabase::populateFontDatabase() {
    const QDir dir(QString::fromLatin1(FONT_DIR));
    const QStringList patterns { QStringLiteral("*.ttf"), QStringLiteral("*.otf") };
    for (const QFileInfo& file : dir.entryInfoList(patterns, QDir::Files)) {
        addTTFile(QByteArray(), QFile::encodeName(file.absoluteFilePath()));
    }
}
