/* sheen: the text editor, for code and for notes, prose and config alike. Each path given opens in a tab. */
#include "editor_window.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("Sheen"));

    editor_window window;
    window.open_paths(QApplication::arguments().mid(1));
    window.show();

    return app.exec();
}
