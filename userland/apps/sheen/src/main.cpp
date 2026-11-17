/* sheen: the text editor, for code and for notes, prose and config alike. Each path given opens in a tab. */
#include "app/instance.h"
#include "app/sheen_app.h"
#include "workspace/sheen_window.h"

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

    QApplication qt_app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("Sheen"));

    sheen::sheen_app app;
    sheen::sheen_window* window = app.create_window();
    window->open_paths(paths);
    window->show();

    return qt_app.exec();
}
