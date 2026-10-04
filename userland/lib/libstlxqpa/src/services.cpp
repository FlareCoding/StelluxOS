#include "services.h"

#include <QByteArray>
#include <QUrl>
#include <cstring>
#include <memory>

#include <stlx/proc.h>
#include <stlxconf/conf.h>

// A handler's own arguments, the URL after them, and the terminating NULL
static constexpr int HANDLER_ARGV_CAPACITY = 16;

bool QStelluxServices::openUrl(const QUrl& url) {
    // A file that cannot be read leaves the defaults, which name no handler
    auto conf = std::make_unique<stlxconf_t>();
    stlxconf_load(conf.get(), STLXCONF_PATH);
    stlxconf_load_drop_ins(conf.get(), STLXCONF_DROP_IN_DIR);

    const QByteArray scheme = url.scheme().toUtf8();
    const stlxconf_url_handler_t* handler = stlxconf_find_url_handler(conf.get(), scheme.constData());
    if (!handler) {
        return false;
    }

    char args[sizeof(handler->args)];
    memcpy(args, handler->args, sizeof(args));
    const char* argv[HANDLER_ARGV_CAPACITY];
    int argc = stlxconf_split_args(args, argv, HANDLER_ARGV_CAPACITY - 1);

    const QByteArray target = url.toEncoded();
    argv[argc++] = target.constData();
    argv[argc] = nullptr;

    int handle = proc_exec(handler->path, argv);
    if (handle < 0) {
        return false;
    }

    proc_detach(handle);
    return true;
}
