/* sheen: the text editor, for code and for notes, prose and config alike. Each path given opens in a tab. */
#include "editor_window.h"
#include "instance.h"
#include "kiln.h"
#include "settings.h"

#include <QApplication>
#include <QFile>

// A running Sheen takes the paths before this launch opens a window of its own
int main(int argc, char** argv) {
    QStringList paths;
    for (int index = 1; index < argc; index++) {
        paths.append(QFile::decodeName(argv[index]));
    }

    if (hand_to_running_instance(paths)) {
        return 0;
    }

    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("Sheen"));

    sheen_settings settings;
    QStringList problems = read_settings(QString::fromLatin1(SETTINGS_PATH), &settings);
    set_text_size(settings.font_size);

    editor_window window(settings, problems);
    instance_server instance;
    instance.on_paths = [&window](const QStringList& handed) { window.receive_paths(handed); };
    instance.listen();

    window.open_paths(paths);
    window.show();

    return app.exec();
}
