#ifndef STLXDM_CAPTURE_ENCODER_HPP
#define STLXDM_CAPTURE_ENCODER_HPP

#include <stlxgfx/surface.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

/* One capture to encode: its pixels, which the encoder takes, and whether their alpha counts */
struct capture_job {
    uint64_t           id = 0;
    stlxgfx_surface_t* pixels = nullptr;
    bool               keeps_alpha = false;
};

/* An encoded capture: the shared memory object holding its PNG, or the errno that kept one from being made, with the
 * job's pixels handed back then so a retry needs no new capture */
struct capture_png {
    uint64_t           id = 0;
    int                object = -1;
    uint64_t           len = 0;
    int                error = 0;
    stlxgfx_surface_t* pixels = nullptr;
    bool               keeps_alpha = false;
};

/* Encodes captures as PNG on a thread of its own, so a large capture never holds up a frame, each into the shared
 * memory object the clipboard keeps. Captures come out in the order they went in. */
class capture_encoder {
public:
    capture_encoder() = default;
    ~capture_encoder();

    capture_encoder(const capture_encoder&) = delete;
    capture_encoder& operator=(const capture_encoder&) = delete;

    /* Starts the thread. False when it or the wake pipe cannot be made. */
    bool start();

    /* Readable while an encoded capture waits for `take()` */
    int wake_fd() const { return m_wake[0]; }

    /* Queues `job`, taking its pixels. False while not started, the pixels destroyed. */
    bool encode(capture_job job);

    /* Empties the wake pipe and moves out the oldest encoded capture, false when none waits */
    bool take(capture_png* png);

private:
    void encode_loop();
    void wake();

    int m_wake[2] = { -1, -1 };

    std::mutex              m_lock;
    std::condition_variable m_job_waiting;
    bool                    m_stopping = false;
    std::deque<capture_job> m_jobs;
    std::deque<capture_png> m_done;

    std::thread m_encoder;
};

#endif
