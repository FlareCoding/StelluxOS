#ifndef STLXDM_SHELL_CLIPBOARD_IMAGES_HPP
#define STLXDM_SHELL_CLIPBOARD_IMAGES_HPP

#include "../clipboard.hpp"

#include <stlxgfx/surface.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/* A decoded picture the cache shares with whatever shows it, destroyed with its last holder */
using shared_picture = std::shared_ptr<stlxgfx_surface_t>;

/* An image entry's pictures: its 32 px thumbnail, and its preview fitted inside the largest pocket a preview has */
struct clipboard_image {
    uint64_t       id = 0;
    bool           is_decoding = false;
    bool           is_decoded = false;    /* a decode finished, whether or not it made pictures */
    bool           is_too_large = false;  /* its header promises more pixels than a preview decodes */
    uint32_t       width = 0;
    uint32_t       height = 0;
    shared_picture thumbnail;
    shared_picture preview;
    uint64_t       used = 0;              /* when its preview was last asked for, which keeps it among the kept few */
};

/* The image entries' thumbnails and previews, decoded on a thread of its own so neither a large capture nor a PNG
 * whose header promises a billion pixels ever holds up a frame. A thumbnail stays while its entry lasts, and only the
 * previews last asked for stay, since each holds about a megabyte. */
class clipboard_images {
public:
    clipboard_images() = default;
    ~clipboard_images();

    clipboard_images(const clipboard_images&) = delete;
    clipboard_images& operator=(const clipboard_images&) = delete;

    /* Starts the thread. False when it or the wake pipe cannot be made. */
    bool start();

    /* Readable while a decoded image waits for `pump()` */
    int wake_fd() const { return m_wake[0]; }

    /* An image entry's pictures, their decode asked for when they are missing: at the back of the queue for a
     * thumbnail, and at its front for the preview of the selected entry */
    clipboard_image of(const clipboard_entry& entry, bool wants_preview);

    /* Empties the wake pipe and keeps the decoded images, true when any arrived */
    bool pump();

    /* Drops the pictures of entries the history no longer keeps */
    void keep(const std::vector<const clipboard_entry*>& entries);

private:
    struct job {
        uint64_t id;
        int      object;  /* a duplicate the decoder closes */
        uint64_t len;
    };

    void decode_loop();
    void wake();
    void ask(clipboard_image& image, const clipboard_entry& entry, bool is_urgent);
    void hurry(uint64_t id);
    void forget_old_previews();

    std::vector<clipboard_image> m_images;
    uint64_t                     m_uses = 0;

    int m_wake[2] = { -1, -1 };

    std::mutex                  m_lock;
    std::condition_variable     m_job_waiting;
    bool                        m_stopping = false;
    std::deque<job>             m_jobs;
    std::deque<clipboard_image> m_done;

    std::thread m_decoder;
};

#endif
