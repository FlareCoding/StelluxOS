/* quickdemo: a Qt Quick program whose animated scene, the default, can log its frame rate,
 * and whose still exact and text scenes let the software and GPU renderers' pictures be compared. */
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickImageProvider>
#include <QUrl>

#include <cstdio>

constexpr int CHECKERBOARD_SIZE = 64;
constexpr int CHECKERBOARD_CELL_SIZE = 8;
constexpr QRgb CHECKERBOARD_DARK = 0xff2a6f97;
constexpr QRgb CHECKERBOARD_LIGHT = 0xfff2c14e;

static const QStringList SCENES { QStringLiteral("animated"), QStringLiteral("exact"), QStringLiteral("text") };

// The exact scene's image, opaque so that a renderer draws it without blending
class checkerboard_provider : public QQuickImageProvider {
public:
    checkerboard_provider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString&, QSize* size, const QSize&) override {
        QImage image(CHECKERBOARD_SIZE, CHECKERBOARD_SIZE, QImage::Format_RGB32);
        for (int y = 0; y < CHECKERBOARD_SIZE; y++) {
            for (int x = 0; x < CHECKERBOARD_SIZE; x++) {
                bool is_dark = ((x / CHECKERBOARD_CELL_SIZE) + (y / CHECKERBOARD_CELL_SIZE)) % 2 == 0;
                image.setPixel(x, y, is_dark ? CHECKERBOARD_DARK : CHECKERBOARD_LIGHT);
            }
        }

        *size = image.size();
        return image;
    }
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    QCommandLineOption scene_option(
        QStringLiteral("scene"),
        QStringLiteral("The scene to show: animated, exact or text."),
        QStringLiteral("name"),
        SCENES.first()
    );
    QCommandLineOption frame_rate_option(
        QStringLiteral("log-frame-rate"),
        QStringLiteral("Log how many frames the animated scene shows each second.")
    );

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(scene_option);
    parser.addOption(frame_rate_option);
    parser.process(app);

    QString scene = parser.value(scene_option);
    if (!SCENES.contains(scene)) {
        std::fprintf(
            stderr,
            "quickdemo: no scene named %s, the scenes are animated, exact and text\n",
            qPrintable(scene)
        );
        return 1;
    }

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("quickdemo"), new checkerboard_provider);
    engine.setInitialProperties({
        { QStringLiteral("scene"), scene },
        { QStringLiteral("logsFrameRate"), parser.isSet(frame_rate_option) },
    });
    engine.load(QUrl(QStringLiteral("qrc:/quickdemo/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return app.exec();
}
