#ifndef SHEEN_FILE_LOADER_H
#define SHEEN_FILE_LOADER_H

/* Reads a file on a worker thread into a Scintilla document no view shows yet, so the window stays responsive while
 * a large file arrives. The worker decodes the bytes once, as the synchronous open does. */
#include "document.h"
#include "encoding.h"

#include <QString>

#include <atomic>
#include <functional>
#include <memory>

namespace Scintilla {
class ILoader;
}

class file_loader;

enum class load_outcome { TEXT, BINARY, UNDECODABLE, FAILED };

struct load_result {
    load_outcome outcome = load_outcome::FAILED;
    file_encoding encoding;
    eol_counts eols;
    QByteArray bytes; // the file's bytes when they look binary, for its placeholder to hold
    QString error;    // why reading the file failed
};

/* What the loader and its worker share, so a cancelled worker can finish on its own after the loader is gone */
struct load_state {
    QString path;
    QByteArray encoding;
    Scintilla::ILoader* loader;
    std::atomic<bool> is_cancelled = false;
    bool is_started = false; // the UI thread's, as is `owner`
    file_loader* owner;
};

class file_loader {
public:
    /* Takes over `loader`, which a view created, until `take_document` hands on the document it filled */
    file_loader(const QString& path, const QByteArray& encoding, Scintilla::ILoader* loader);

    /* Cancels the worker without waiting for it, which then drops the document on its own */
    ~file_loader();

    void start();

    /* The filled document after a TEXT outcome, holding one reference for the caller */
    sptr_t take_document();

    std::function<void(qint64 done, qint64 total)> on_progress;
    std::function<void(const load_result& result)> on_finished;

private:
    std::shared_ptr<load_state> m_state;
};

#endif
