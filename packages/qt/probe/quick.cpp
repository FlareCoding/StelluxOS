#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    engine.loadFromModule("Probe", "Main");

    return app.exec();
}
