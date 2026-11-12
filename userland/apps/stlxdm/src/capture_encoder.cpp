#include "capture_encoder.hpp"

#include "shared_object.hpp"

#include <stlxgfx/image.h>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

capture_encoder::~capture_encoder() {
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stopping = true;
    }

    m_job_waiting.notify_all();
    if (m_encoder.joinable()) {
        m_encoder.join();
    }

    for (capture_job& job : m_jobs) {
        stlxgfx_destroy_surface(job.pixels);
    }

    for (capture_png& png : m_done) {
        stlxgfx_destroy_surface(png.pixels);
        if (png.object >= 0) {
            close(png.object);
        }
    }

    for (int fd : m_wake) {
        if (fd >= 0) {
            close(fd);
        }
    }
}

bool capture_encoder::start() {
    if (pipe(m_wake) != 0) {
        m_wake[0] = m_wake[1] = -1;
        return false;
    }

    fcntl(m_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wake[1], F_SETFL, O_NONBLOCK);

    try {
        m_encoder = std::thread(&capture_encoder::encode_loop, this);
    } catch (const std::system_error&) {
        return false;
    }

    return true;
}

bool capture_encoder::encode(capture_job job) {
    if (!m_encoder.joinable()) {
        stlxgfx_destroy_surface(job.pixels);
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_jobs.push_back(job);
    }

    m_job_waiting.notify_one();
    return true;
}

bool capture_encoder::drop(uint64_t id) {
    std::lock_guard<std::mutex> guard(m_lock);
    for (auto job = m_jobs.begin(); job != m_jobs.end(); ++job) {
        if (job->id == id) {
            stlxgfx_destroy_surface(job->pixels);
            m_jobs.erase(job);
            return true;
        }
    }

    return false;
}

size_t capture_encoder::waiting() {
    std::lock_guard<std::mutex> guard(m_lock);
    return m_jobs.size();
}

bool capture_encoder::take(capture_png* png) {
    char drained[64];
    while (read(m_wake[0], drained, sizeof(drained)) > 0) {
    }

    std::lock_guard<std::mutex> guard(m_lock);
    if (m_done.empty()) {
        return false;
    }

    *png = m_done.front();
    m_done.pop_front();

    return true;
}

/* One byte is enough to wake the compositor, and a full pipe already will */
void capture_encoder::wake() {
    char byte = 1;
    ssize_t written = write(m_wake[1], &byte, 1);
    (void)written;
}

void capture_encoder::encode_loop() {
    std::unique_lock<std::mutex> lock(m_lock);
    for (;;) {
        m_job_waiting.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
        if (m_stopping) {
            return;
        }

        capture_job job = m_jobs.front();
        m_jobs.pop_front();
        lock.unlock();

        size_t len = 0;
        unsigned char* bytes = stlxgfx_encode_png(job.pixels, job.keeps_alpha ? 1 : 0, &len);

        capture_png png = { job.id, -1, len, ENOMEM, nullptr, job.keeps_alpha };
        if (bytes) {
            png.object = make_shared_object(bytes, len);
            png.error = png.object >= 0 ? 0 : errno;
            free(bytes);
        }

        if (png.object >= 0) {
            stlxgfx_destroy_surface(job.pixels);
        } else {
            png.pixels = job.pixels;
        }

        lock.lock();
        m_done.push_back(png);
        wake();
    }
}
