/* sheen: the text editor, for code and for notes, prose and config alike. Each path given opens in a tab. */
#include "editor_window.h"
#include "kiln.h"
#include "settings.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("Sheen"));

    sheen_settings settings;
    QStringList problems = read_settings(QString::fromLatin1(SETTINGS_PATH), &settings);
    set_text_size(settings.font_size);

    editor_window window(settings, problems);
    window.open_paths(QApplication::arguments().mid(1));
    window.show();

    return app.exec();
}
