#ifndef MQ_H
#define MQ_H
#include <kstl/hashmap.h>
#include <string.h>
#include <sync.h>

#define MESSAGE_QUEUE_ID_INVALID 0

namespace ipc {
using mq_handle_t = uint64_t;

struct mq_message {
    uint64_t message_id;
    size_t payload_size;
    uint8_t* payload;
};

struct mq_node {
    mq_message* message;
    mq_node* next;
};

class message_queue {
public:
    // Public facing API
    static mq_handle_t create(const kstl::string& name);
    static mq_handle_t open(const kstl::string& name);

    static bool post_message(mq_handle_t handle, mq_message* message);
    static bool peek_message(mq_handle_t handle);
    static bool get_message(mq_handle_t handle, mq_message* out_message);

private:
    static mq_handle_t s_available_mq_id;
    static mutex s_queue_map_lock;

    static kstl::hashmap<kstl::string, mq_handle_t> s_mq_name_mappings;
    static kstl::hashmap<mq_handle_t, kstl::shared_ptr<message_queue>> s_message_queue_map;

    static message_queue* _get_mq_object(mq_handle_t handle);
    
    // Private API
    mq_node* m_head = nullptr;
    mq_node* m_tail = nullptr;
    size_t m_message_count = 0;
    mutex m_lock;
    uint64_t m_next_message_id = 1;

    bool _post_message(mq_message* message);
    bool _peek_message();
    bool _get_message(mq_message* out_message);
};
} // namespace ipc

#endif // MQ_H

