#ifndef GPUTEST_VENUS_H
#define GPUTEST_VENUS_H

// Venus commands in the layout of venus's protocol for vk.xml 1.4.334, each Vulkan handle a 64-bit id the caller
// picks. A command that replies writes its reply where the context's reply stream points

#include <stdbool.h>
#include <stdint.h>

#define VENUS_LIST_WORDS 128 // room past the longest list gputest sends, a reply stream's setting and vkCreateDevice

#define VENUS_CREATE_INSTANCE            0u
#define VENUS_ENUMERATE_PHYSICAL_DEVICES 2u
#define VENUS_GET_MEMORY_PROPERTIES      8u
#define VENUS_CREATE_DEVICE              11u
#define VENUS_ALLOCATE_MEMORY            21u
#define VENUS_CREATE_FENCE               35u
#define VENUS_WAIT_FOR_FENCES            39u

#define VENUS_SUCCESS 0 // VK_SUCCESS
#define VENUS_TIMEOUT 2 // VK_TIMEOUT

// A command list, its first `count` words encoded
struct venus_list {
    uint32_t words[VENUS_LIST_WORDS];
    uint32_t count;
};

// Points the context's reply stream at `size` bytes from `offset` in virtio resource `resource`, which must be a blob
// of venus's shared memory mapped into the context
void venus_set_reply_stream(struct venus_list* list, uint32_t resource, uint64_t offset, uint64_t size);

void venus_create_instance(struct venus_list* list, uint64_t instance);

// Names the instance's first physical device `physical_device`
void venus_enumerate_physical_devices(struct venus_list* list, uint64_t instance, uint64_t physical_device);

void venus_get_memory_properties(struct venus_list* list, uint64_t physical_device);

// A device with one queue of family 0
void venus_create_device(struct venus_list* list, uint64_t physical_device, uint64_t device);

void venus_allocate_memory(struct venus_list* list, uint64_t device, uint64_t memory, uint64_t size, uint32_t type);
void venus_free_memory(struct venus_list* list, uint64_t device, uint64_t memory);
void venus_create_fence(struct venus_list* list, uint64_t device, uint64_t fence);

// Waits for `fence` at most `timeout_ns`, holding the host's decoder that long for a fence nothing signals
void venus_wait_for_fence(struct venus_list* list, uint64_t device, uint64_t fence, uint64_t timeout_ns);

// Reads the VkResult of a reply of `command`. Returns false while `reply` holds none
bool venus_result(const volatile uint32_t* reply, uint32_t command, int32_t* result);

// The first memory type both host visible and host coherent in a reply of `VENUS_GET_MEMORY_PROPERTIES`, or -1
int venus_host_visible_type(const volatile uint32_t* reply);

#endif // GPUTEST_VENUS_H
