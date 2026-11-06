// Encodes the venus commands gputest sends, each argument in the order and width venus's decoder reads it

#include "venus.h"

#define FLAG_REPLY 1u

// Numbers of the commands sent without a reply
#define FREE_MEMORY      22u
#define SET_REPLY_STREAM 178u

#define STYPE_INSTANCE_CREATE_INFO     1u
#define STYPE_DEVICE_QUEUE_CREATE_INFO 2u
#define STYPE_DEVICE_CREATE_INFO       3u
#define STYPE_MEMORY_ALLOCATE_INFO     5u
#define STYPE_FENCE_CREATE_INFO        8u

// A pointer is encoded as whether what it points at follows, and an array as its length, then its elements. A length
// other than the one the decoder expects makes the context's decoder fatal
#define PRESENT 1u
#define ABSENT  0u

#define MAX_MEMORY_TYPES 32u
#define MAX_MEMORY_HEAPS 16u

#define MEMORY_HOST_VISIBLE  0x2u
#define MEMORY_HOST_COHERENT 0x4u

// Word indices from a memory properties reply's start, past the command word and the pointer's two words. The type
// count and the array's length come first, then each type's flags and heap
#define TYPE_COUNT_WORD 3
#define FIRST_TYPE_WORD 6

static void put32(struct venus_list* list, uint32_t value) {
    list->words[list->count] = value;
    list->count++;
}

static void put64(struct venus_list* list, uint64_t value) {
    put32(list, (uint32_t)value);
    put32(list, (uint32_t)(value >> 32));
}

static void begin(struct venus_list* list, uint32_t command, uint32_t flags) {
    put32(list, command);
    put32(list, flags);
}

// Ends a create: no allocator, then the id the caller picked for the object
static void put_created(struct venus_list* list, uint64_t id) {
    put64(list, ABSENT);
    put64(list, PRESENT);
    put64(list, id);
}

void venus_set_reply_stream(struct venus_list* list, uint32_t resource, uint64_t offset, uint64_t size) {
    begin(list, SET_REPLY_STREAM, 0);
    put64(list, PRESENT);
    put32(list, resource);
    put64(list, offset);
    put64(list, size);
}

void venus_create_instance(struct venus_list* list, uint64_t instance) {
    begin(list, VENUS_CREATE_INSTANCE, FLAG_REPLY);
    put64(list, PRESENT);
    put32(list, STYPE_INSTANCE_CREATE_INFO);
    put64(list, ABSENT);
    put32(list, 0);

    put64(list, ABSENT);
    put32(list, 0);
    put64(list, 0);
    put32(list, 0);
    put64(list, 0);

    put_created(list, instance);
}

void venus_enumerate_physical_devices(struct venus_list* list, uint64_t instance, uint64_t physical_device) {
    begin(list, VENUS_ENUMERATE_PHYSICAL_DEVICES, FLAG_REPLY);
    put64(list, instance);
    put64(list, PRESENT);
    put32(list, 1);
    put64(list, 1);
    put64(list, physical_device);
}

void venus_get_memory_properties(struct venus_list* list, uint64_t physical_device) {
    begin(list, VENUS_GET_MEMORY_PROPERTIES, FLAG_REPLY);
    put64(list, physical_device);
    put64(list, PRESENT);
    put64(list, MAX_MEMORY_TYPES);
    put64(list, MAX_MEMORY_HEAPS);
}

void venus_create_device(struct venus_list* list, uint64_t physical_device, uint64_t device) {
    union {
        float value;
        uint32_t bits;
    } priority = {1.0f};

    begin(list, VENUS_CREATE_DEVICE, FLAG_REPLY);
    put64(list, physical_device);

    put64(list, PRESENT);
    put32(list, STYPE_DEVICE_CREATE_INFO);
    put64(list, ABSENT);
    put32(list, 0);
    put32(list, 1);
    put64(list, 1);

    put32(list, STYPE_DEVICE_QUEUE_CREATE_INFO);
    put64(list, ABSENT);
    put32(list, 0);

    put32(list, 0);
    put32(list, 1);
    put64(list, 1);
    put32(list, priority.bits);

    put32(list, 0);
    put64(list, 0);
    put32(list, 0);
    put64(list, 0);
    put64(list, ABSENT);
    put_created(list, device);
}

void venus_allocate_memory(struct venus_list* list, uint64_t device, uint64_t memory, uint64_t size, uint32_t type) {
    begin(list, VENUS_ALLOCATE_MEMORY, FLAG_REPLY);
    put64(list, device);

    put64(list, PRESENT);
    put32(list, STYPE_MEMORY_ALLOCATE_INFO);
    put64(list, ABSENT);
    put64(list, size);
    put32(list, type);
    put_created(list, memory);
}

void venus_free_memory(struct venus_list* list, uint64_t device, uint64_t memory) {
    begin(list, FREE_MEMORY, 0);
    put64(list, device);
    put64(list, memory);
    put64(list, ABSENT);
}

void venus_create_fence(struct venus_list* list, uint64_t device, uint64_t fence) {
    begin(list, VENUS_CREATE_FENCE, FLAG_REPLY);
    put64(list, device);

    put64(list, PRESENT);
    put32(list, STYPE_FENCE_CREATE_INFO);
    put64(list, ABSENT);
    put32(list, 0);
    put_created(list, fence);
}

void venus_wait_for_fence(struct venus_list* list, uint64_t device, uint64_t fence, uint64_t timeout_ns) {
    begin(list, VENUS_WAIT_FOR_FENCES, FLAG_REPLY);
    put64(list, device);

    put32(list, 1);
    put64(list, 1);
    put64(list, fence);
    put32(list, 1);
    put64(list, timeout_ns);
}

bool venus_result(const volatile uint32_t* reply, uint32_t command, int32_t* result) {
    if (reply[0] != command) {
        return false;
    }

    *result = (int32_t)reply[1];

    return true;
}

int venus_host_visible_type(const volatile uint32_t* reply) {
    if (reply[0] != VENUS_GET_MEMORY_PROPERTIES) {
        return -1;
    }

    uint32_t wanted = MEMORY_HOST_VISIBLE | MEMORY_HOST_COHERENT;
    uint32_t count = reply[TYPE_COUNT_WORD];
    for (uint32_t i = 0; i < count && i < MAX_MEMORY_TYPES; i++) {
        if ((reply[FIRST_TYPE_WORD + 2 * i] & wanted) == wanted) {
            return (int)i;
        }
    }

    return -1;
}
