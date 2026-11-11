#include "file_loader.h"

#include <QCoreApplication>
#include <QFile>
#include <QThread>

#include <ILoader.h>

// Large enough to keep the worker's calls few, small enough for the progress to move and a cancel to land soon
static constexpr qint64 READ_CHUNK_BYTES = 4 * 1024 * 1024;
static constexpr qsizetype FILL_CHUNK_BYTES = 4 * 1024 * 1024;

// Posted to the application, which outlives every loader, and run only while the loader that asked is still there
static void post_progress(const std::shared_ptr<load_state>& state, qint64 done, qint64 total) {
    if (state->is_cancelled) {
        return;
    }

    QMetaObject::invokeMethod(qApp, [state, done, total] {
        std::function<void(qint64, qint64)> progress = state->owner ? state->owner->on_progress : nullptr;
        if (progress) {
            progress(done, total);
        }
    }, Qt::QueuedConnection);
}

// A document filled for a loader that is gone has nobody to take it, so it is dropped here
static void post_result(const std::shared_ptr<load_state>& state, const load_result& result) {
    QMetaObject::invokeMethod(qApp, [state, result] {
        if (!state->owner) {
            if (state->loader) {
                state->loader->Release();
            }

            return;
        }

        std::function<void(const load_result&)> finished = state->owner->on_finished;
        if (finished) {
            finished(result);
        }
    }, Qt::QueuedConnection);
}

// Only a TEXT outcome keeps the document, for the loader to take. A cancelled worker posts nothing, since the window
// may be closing with the application behind it
static void run(const std::shared_ptr<load_state>& state) {
    auto stop = [&state](const load_result& result) {
        state->loader->Release();
        state->loader = nullptr;
        if (!state->is_cancelled) {
            post_result(state, result);
        }
    };

    QFile file(state->path);
    if (!file.open(QIODevice::ReadOnly)) {
        stop({ load_outcome::FAILED, {}, {}, {}, file.errorString() });
        return;
    }

    qint64 size = qMax<qint64>(file.size(), 1);
    QByteArray bytes;
    bytes.reserve(file.size());
    while (!file.atEnd()) {
        if (state->is_cancelled) {
            stop({});
            return;
        }

        QByteArray chunk = file.read(READ_CHUNK_BYTES);
        if (file.error() != QFileDevice::NoError) {
            stop({ load_outcome::FAILED, {}, {}, {}, file.errorString() });
            return;
        }

        bytes += chunk;
        post_progress(state, bytes.size(), 2 * size);
    }

    if (state->encoding.isEmpty() && looks_binary(bytes)) {
        stop({ load_outcome::BINARY, {}, {}, bytes, {} });
        return;
    }

    decoded_text text;
    if (state->encoding.isEmpty()) {
        text = decode_detected(bytes);
    } else if (!decode_as(bytes, state->encoding, &text)) {
        stop({ load_outcome::UNDECODABLE, { state->encoding, false }, {}, {}, {} });
        return;
    }

    bytes = QByteArray();
    qsizetype length = text.utf8.size();
    for (qsizetype offset = 0; offset < length; offset += FILL_CHUNK_BYTES) {
        if (state->is_cancelled) {
            stop({});
            return;
        }

        qsizetype chunk = qMin(FILL_CHUNK_BYTES, length - offset);
        if (state->loader->AddData(text.utf8.constData() + offset, chunk) != SC_STATUS_OK) {
            stop({ load_outcome::FAILED, {}, {}, {}, QStringLiteral("out of memory") });
            return;
        }

        double filled = static_cast<double>(offset + chunk) / static_cast<double>(length);
        post_progress(state, size + static_cast<qint64>(filled * static_cast<double>(size)), 2 * size);
    }

    if (state->is_cancelled) {
        stop({});
        return;
    }

    post_result(state, { load_outcome::TEXT, text.encoding, count_eols(text.utf8), {}, {} });
}

file_loader::file_loader(const QString& path, const QByteArray& encoding, Scintilla::ILoader* loader)
    : m_state(std::make_shared<load_state>()) {
    m_state->path = path;
    m_state->encoding = encoding;
    m_state->loader = loader;
    m_state->owner = this;
}

// A loader never started still holds its document, since no worker ran to drop it
file_loader::~file_loader() {
    m_state->owner = nullptr;
    m_state->is_cancelled = true;
    if (!m_state->is_started && m_state->loader) {
        m_state->loader->Release();
        m_state->loader = nullptr;
    }
}

// The thread deletes itself once the worker returns, whether or not its loader is still there
void file_loader::start() {
    m_state->is_started = true;
    std::shared_ptr<load_state> state = m_state;
    QThread* thread = QThread::create([state] { run(state); });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

sptr_t file_loader::take_document() {
    sptr_t document = reinterpret_cast<sptr_t>(m_state->loader->ConvertToDocument());
    m_state->loader = nullptr;
    return document;
}
