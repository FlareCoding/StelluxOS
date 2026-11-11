#include "services.h"

#include <QByteArray>
#include <QFile>
#include <QMimeDatabase>
#include <QUrl>
#include <cstring>
#include <memory>

#include <stlx/proc.h>
#include <stlxconf/conf.h>

// A handler's own arguments, the URL or path after them, and the terminating NULL
static constexpr int HANDLER_ARGV_CAPACITY = 16;

// The type of an empty file whose name says nothing, which opens as text since it holds nothing else
static constexpr QLatin1StringView EMPTY_FILE_TYPE("application/x-zerosize");
static constexpr QLatin1StringView TEXT_TYPE("text/plain");

// A file that cannot be read leaves the defaults, which name no handler
static std::unique_ptr<stlxconf_t> load_desktop_conf() {
    auto conf = std::make_unique<stlxconf_t>();
    stlxconf_load(conf.get(), STLXCONF_PATH);
    stlxconf_load_drop_ins(conf.get(), STLXCONF_DROP_IN_DIR);

    return conf;
}

template <typename handler_t>
static bool start_handler(const handler_t& handler, const QByteArray& target) {
    char args[sizeof(handler.args)];
    memcpy(args, handler.args, sizeof(args));
    const char* argv[HANDLER_ARGV_CAPACITY];
    int argc = stlxconf_split_args(args, argv, HANDLER_ARGV_CAPACITY - 1);

    argv[argc++] = target.constData();
    argv[argc] = nullptr;

    int handle = proc_exec(handler.path, argv);
    if (handle < 0) {
        return false;
    }

    proc_detach(handle);
    return true;
}

bool QStelluxServices::openUrl(const QUrl& url) {
    if (url.isLocalFile()) {
        return openDocument(url);
    }

    auto conf = load_desktop_conf();
    const QByteArray scheme = url.scheme().toUtf8();
    const stlxconf_url_handler_t* handler = stlxconf_find_url_handler(conf.get(), scheme.constData());

    return handler && start_handler(*handler, url.toEncoded());
}

// The file's own type is tried first, then each type it derives from, so C source opens in a text/plain handler
bool QStelluxServices::openDocument(const QUrl& url) {
    if (!url.isLocalFile()) {
        return false;
    }

    const QString path = url.toLocalFile();
    QMimeDatabase types;
    QMimeType type = types.mimeTypeForFile(path);
    if (type.name() == EMPTY_FILE_TYPE) {
        type = types.mimeTypeForName(TEXT_TYPE);
    }

    auto conf = load_desktop_conf();
    const QStringList candidates = QStringList(type.name()) + type.allAncestors();
    for (const QString& candidate : candidates) {
        const QByteArray name = candidate.toUtf8();
        const stlxconf_file_handler_t* handler = stlxconf_find_file_handler(conf.get(), name.constData());
        if (handler) {
            return start_handler(*handler, QFile::encodeName(path));
        }
    }

    return false;
}
