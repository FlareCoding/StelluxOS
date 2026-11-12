#include "offer_intake.hpp"

#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

bool queue_received_handles(handle_queue queue, const msghdr& msg) {
    bool kept_all = (msg.msg_flags & MSG_CTRUNC) == 0;

    const cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len < CMSG_LEN(0)) {
        return kept_all;
    }

    size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (size_t i = 0; i < count; i++) {
        int handle;
        memcpy(&handle, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(handle));
        if (*queue.count == queue.capacity) {
            close(handle);
            kept_all = false;
            continue;
        }

        queue.handles[(*queue.count)++] = handle;
    }

    return kept_all;
}

bool may_begin_handle_message(const uint8_t* bytes, uint32_t have) {
    if (have == 0) {
        return false;
    }

    if (have < sizeof(uint16_t)) {
        return true;
    }

    uint16_t type;
    memcpy(&type, bytes, sizeof(type));

    return type == SWP_MSG_ATTACH_GPU_BUFFER || type == SWP_MSG_CLIPBOARD_SET || type == SWP_MSG_CLIPBOARD_OFFER ||
           type == SWP_MSG_START_DRAG;
}

/* A representation's object must be regular and hold at least its length, and it records what the object holds */
static intake_error check_object(clipboard_rep& rep) {
    struct stat st;
    if (fstat(rep.object, &st) != 0 || !S_ISREG(st.st_mode)) {
        return intake_error::not_memory;
    }

    uint64_t size = static_cast<uint64_t>(st.st_size);
    if (size < rep.len) {
        return intake_error::short_object;
    }

    rep.held = size;
    return intake_error::none;
}

intake_error take_offer_objects(std::vector<clipboard_rep>& reps, handle_queue queue) {
    uint32_t needed = 0;
    for (const clipboard_rep& rep : reps) {
        needed += rep.len > 0 ? 1 : 0;
    }

    if (*queue.count < needed) {
        return intake_error::missing_handles;
    }

    uint32_t taken = 0;
    for (clipboard_rep& rep : reps) {
        if (rep.len > 0) {
            rep.object = queue.handles[taken++];
        }
    }

    *queue.count -= taken;
    memmove(queue.handles, queue.handles + taken, *queue.count * sizeof(int));

    intake_error error = intake_error::none;
    for (clipboard_rep& rep : reps) {
        if (rep.object >= 0 && error == intake_error::none) {
            error = check_object(rep);
        }
    }

    for (clipboard_rep& rep : reps) {
        if (error != intake_error::none && rep.object >= 0) {
            close(rep.object);
            rep.object = -1;
        }
    }

    return error;
}

/* A new object holding a representation's bytes, -1 when its object refuses the mapping or no object can be made */
static int copy_object(const clipboard_rep& rep, const object_maker& make) {
    void* bytes = mmap(nullptr, rep.len, PROT_READ, MAP_SHARED, rep.object, 0);
    if (bytes == MAP_FAILED) {
        return -1;
    }

    int object = make(bytes, rep.len);
    munmap(bytes, rep.len);
    return object;
}

void own_small_item(std::vector<clipboard_rep>& reps, const object_maker& make) {
    uint64_t total = 0;
    for (const clipboard_rep& rep : reps) {
        total += rep.len;
    }

    if (total > OWNED_COPY_MAX) {
        return;
    }

    std::vector<int> owned;
    for (const clipboard_rep& rep : reps) {
        int object = rep.len > 0 ? copy_object(rep, make) : -1;
        if (rep.len > 0 && object < 0) {
            for (int made : owned) {
                if (made >= 0) {
                    close(made);
                }
            }

            return;
        }

        owned.push_back(object);
    }

    for (size_t i = 0; i < reps.size(); i++) {
        if (reps[i].object >= 0) {
            close(reps[i].object);
        }

        reps[i].object = owned[i];
        reps[i].held = reps[i].len;
    }
}

bool holds_length(const clipboard_rep& rep) {
    struct stat st;
    return rep.object >= 0 && fstat(rep.object, &st) == 0 && static_cast<uint64_t>(st.st_size) >= rep.len;
}

const char* intake_error_text(intake_error error) {
    switch (error) {
    case intake_error::none: return "held up";
    case intake_error::missing_handles: return "came without their handles";
    case intake_error::not_memory: return "were not shared memory";
    case intake_error::short_object: return "held less than their lengths";
    }

    return "were refused";
}
