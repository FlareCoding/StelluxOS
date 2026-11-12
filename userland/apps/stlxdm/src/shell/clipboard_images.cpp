#include "clipboard_images.hpp"

#include "../free_memory.hpp"

#include <stlxgfx/image.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <fcntl.h>
#include <string_view>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>

/* A thumbnail is a row's icon, and a preview fits inside the largest pocket a preview has, so painting it only shrinks
 * it */
constexpr uint32_t THUMBNAIL_SIZE = STLXTHEME_DOCK_ICON_SIZE;
constexpr uint32_t PREVIEW_MAX_W = 640;
constexpr uint32_t PREVIEW_MAX_H = 400;
constexpr size_t PREVIEWS_KEPT = 4;

static shared_picture shared(stlxgfx_surface_t* picture) {
    return picture ? shared_picture(picture, stlxgfx_destroy_surface) : shared_picture();
}

/* A thumbnail covers its square with the middle of the picture, as the history's mock crops a wide capture */
static stlxgfx_surface_t* cover(const stlxgfx_surface_t* picture) {
    uint32_t side = std::min(picture->width, picture->height);
    stlxgfx_surface_t* thumbnail = stlxgfx_create_surface(THUMBNAIL_SIZE, THUMBNAIL_SIZE, 32, 16, 8, 0);
    if (thumbnail) {
        stlxgfx_blit_scaled(thumbnail, 0, 0, THUMBNAIL_SIZE, THUMBNAIL_SIZE, picture,
                            static_cast<int32_t>((picture->width - side) / 2),
                            static_cast<int32_t>((picture->height - side) / 2), side, side);
    }

    return thumbnail;
}

/* The picture shrunk to fit a preview's box, never grown */
static stlxgfx_surface_t* fitted(const stlxgfx_surface_t* picture) {
    double scale = std::min({ 1.0, static_cast<double>(PREVIEW_MAX_W) / picture->width,
                              static_cast<double>(PREVIEW_MAX_H) / picture->height });
    uint32_t w = std::max(1u, static_cast<uint32_t>(picture->width * scale));
    uint32_t h = std::max(1u, static_cast<uint32_t>(picture->height * scale));
    stlxgfx_surface_t* preview = stlxgfx_create_surface(w, h, 32, 16, 8, 0);
    if (preview) {
        stlxgfx_blit_scaled(preview, 0, 0, w, h, picture, 0, 0, picture->width, picture->height);
    }

    return preview;
}

/* A decode holds the inflated rows and the pixels, four bytes a pixel each */
static uint64_t decode_bytes(uint32_t width, uint32_t height) {
    return 2 * static_cast<uint64_t>(width) * height * 4;
}

/* The header is read first, so a picture past what a preview decodes is never decoded at all, nor one free memory
 * cannot hold, which keeps the placeholder */
static clipboard_image decode(uint64_t id, int object, uint64_t len) {
    clipboard_image image;
    image.id = id;
    void* bytes = len > 0 ? mmap(nullptr, len, PROT_READ, MAP_SHARED, object, 0) : MAP_FAILED;
    if (bytes == MAP_FAILED) {
        return image;
    }

    std::string_view header(static_cast<const char*>(bytes), std::min<uint64_t>(len, 24));
    bool has_size = png_size(header, &image.width, &image.height);
    image.is_too_large = has_size && !fits_preview(image.width, image.height);

    bool can_decode = has_size && !image.is_too_large && has_memory_for(decode_bytes(image.width, image.height));
    stlxgfx_surface_t* picture = can_decode ? stlxgfx_decode_image(bytes, len) : nullptr;
    munmap(bytes, len);
    if (picture) {
        image.thumbnail = shared(cover(picture));
        image.preview = shared(fitted(picture));
        stlxgfx_destroy_surface(picture);
    }

    return image;
}

clipboard_images::~clipboard_images() {
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stopping = true;
    }

    m_job_waiting.notify_all();
    if (m_decoder.joinable()) {
        m_decoder.join();
    }

    for (const job& waiting : m_jobs) {
        close(waiting.object);
    }

    for (int fd : m_wake) {
        if (fd >= 0) {
            close(fd);
        }
    }
}

bool clipboard_images::start() {
    if (pipe(m_wake) != 0) {
        m_wake[0] = m_wake[1] = -1;
        return false;
    }

    fcntl(m_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wake[1], F_SETFL, O_NONBLOCK);

    try {
        m_decoder = std::thread(&clipboard_images::decode_loop, this);
    } catch (const std::system_error&) {
        return false;
    }

    return true;
}

clipboard_image clipboard_images::of(const clipboard_entry& entry, bool wants_preview) {
    auto found = std::find_if(m_images.begin(), m_images.end(), [&entry](const clipboard_image& image) {
        return image.id == entry.id;
    });

    if (found == m_images.end()) {
        m_images.push_back({});
        m_images.back().id = entry.id;
        found = m_images.end() - 1;
    }

    clipboard_image& image = *found;
    if (wants_preview) {
        image.used = ++m_uses;
    }

    bool was_dropped = wants_preview && image.thumbnail && !image.preview;
    if (!image.is_decoding && (!image.is_decoded || was_dropped)) {
        ask(image, entry, wants_preview);
    } else if (image.is_decoding && wants_preview) {
        hurry(entry.id);
    }

    return image;
}

/* The decoder reads a duplicate of the PNG's object, so an entry removed meanwhile leaves the decode its own reference,
 * and an image that cannot be asked for counts as decoded into nothing */
void clipboard_images::ask(clipboard_image& image, const clipboard_entry& entry, bool is_urgent) {
    const clipboard_rep* png = entry.item.find("image/png");
    int object = png && m_decoder.joinable() ? fcntl(png->object, F_DUPFD_CLOEXEC, 0) : -1;
    if (object < 0) {
        image.is_decoded = true;
        return;
    }

    {
        std::lock_guard<std::mutex> guard(m_lock);
        job next = { entry.id, object, png->len };
        if (is_urgent) {
            m_jobs.push_front(next);
        } else {
            m_jobs.push_back(next);
        }
    }

    image.is_decoding = true;
    m_job_waiting.notify_one();
}

/* A decode queued for a thumbnail moves to the front once its entry is selected, unless the decoder already took it */
void clipboard_images::hurry(uint64_t id) {
    std::lock_guard<std::mutex> guard(m_lock);
    auto queued = std::find_if(m_jobs.begin(), m_jobs.end(), [id](const job& waiting) {
        return waiting.id == id;
    });

    if (queued != m_jobs.end()) {
        std::rotate(m_jobs.begin(), queued, queued + 1);
    }
}

bool clipboard_images::pump() {
    char drained[64];
    while (read(m_wake[0], drained, sizeof(drained)) > 0) {
    }

    std::deque<clipboard_image> done;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        done.swap(m_done);
    }

    for (clipboard_image& decoded : done) {
        auto found = std::find_if(m_images.begin(), m_images.end(), [&decoded](const clipboard_image& image) {
            return image.id == decoded.id;
        });

        if (found == m_images.end()) {
            continue;
        }

        found->is_decoding = false;
        found->is_decoded = true;
        found->is_too_large = decoded.is_too_large;
        found->width = decoded.width;
        found->height = decoded.height;

        found->thumbnail = decoded.thumbnail ? decoded.thumbnail : found->thumbnail;
        found->preview = decoded.preview;
    }

    forget_old_previews();
    return !done.empty();
}

/* Only the previews asked for last stay, and a preview still shown stays with whatever shows it */
void clipboard_images::forget_old_previews() {
    std::vector<clipboard_image*> previewed;
    for (clipboard_image& image : m_images) {
        if (image.preview) {
            previewed.push_back(&image);
        }
    }

    std::sort(previewed.begin(), previewed.end(), [](const clipboard_image* a, const clipboard_image* b) {
        return a->used > b->used;
    });

    for (size_t i = PREVIEWS_KEPT; i < previewed.size(); i++) {
        previewed[i]->preview.reset();
    }
}

void clipboard_images::keep(const std::vector<const clipboard_entry*>& entries) {
    std::erase_if(m_images, [&entries](const clipboard_image& image) {
        return std::none_of(entries.begin(), entries.end(), [&image](const clipboard_entry* entry) {
            return entry->id == image.id;
        });
    });
}

/* One byte is enough to wake the compositor, and a full pipe already will */
void clipboard_images::wake() {
    char byte = 1;
    ssize_t written = write(m_wake[1], &byte, 1);
    (void)written;
}

void clipboard_images::decode_loop() {
    std::unique_lock<std::mutex> lock(m_lock);
    for (;;) {
        m_job_waiting.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
        if (m_stopping) {
            return;
        }

        job next = m_jobs.front();
        m_jobs.pop_front();
        lock.unlock();

        clipboard_image image = decode(next.id, next.object, next.len);
        close(next.object);

        lock.lock();
        m_done.push_back(std::move(image));
        wake();
    }
}
