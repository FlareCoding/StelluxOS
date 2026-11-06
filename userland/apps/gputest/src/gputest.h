#ifndef GPUTEST_H
#define GPUTEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stlxwin/proto.h>

#define PROTO_WINDOW_ID 1u  // the window id proto_create_window sends

#define NS_PER_SEC   1000000000ULL
#define MIB          (1024ULL * 1024ULL)
#define PAGE_BYTES   4096ULL
#define BUFFER_BYTES MIB
#define POLL_US      1000
#define FILL_WORD    0xA5C3E10Fu

// Prints a PASS or FAIL line for one check and counts it
void check(const char* name, bool cond);

uint64_t monotonic_ns(void);

// Returns 0 or the call's errno
int gpu_call(int fd, uint32_t cmd, void* arg);

// Maps a buffer's first BUFFER_BYTES for the CPU, or returns null
volatile uint32_t* map_for_cpu(int32_t buffer);

// Whether every word of a buffer's first BUFFER_BYTES reads as `value`
bool reads_as(const volatile uint32_t* words, uint32_t value);

// Writes `value` to every word of a buffer's first BUFFER_BYTES
void fill(volatile uint32_t* words, uint32_t value);

// Unmaps the CPU's words when given and closes the handle when it is one
void release(int32_t buffer, volatile uint32_t* words);

// Returns a new context's handle, or -1
int32_t create_context(int fd);

// The window protocol spoken without libstlxwin, to break rules the library keeps or cannot check. Connects and runs
// the handshake, filling `gpu_info` from the GPU_INFO the reply announces. Returns the socket or -1
int proto_connect(swp_gpu_info* gpu_info);

// Sends one message in one sendmsg, with `count` handles in its control data
bool proto_send(int sock, uint16_t type, const void* payload, uint32_t length, const int32_t* handles, uint32_t count);

// Sends CREATE_WINDOW for a window of 64 by 64 pixels, with `count` handles
bool proto_create_window(int sock, const int32_t* handles, uint32_t count);

// Reads whatever stlxdm sends until it closes the socket. Returns false if it is still open after 2 seconds
bool proto_closes(int sock);

// The window protocol's checks, which speak it over sockets of their own and need no GPU
void run_proto_checks(void);

// The window checks, which commit GPU buffers of the device `gpu`
void run_window_checks(int gpu);

// The resize check, run alone as `gputest resize`, since stlxdm cascades each window by how many it made before and the
// card job drags at that place. Once its prompt prints, it waits two minutes for the job to drag its right edge
void run_resize_check(int gpu);

// Presents on gfxfb's output 0 in stlxdm's place, from a thread of its own, until stop_presenting() or until a present
// call or the read of its end fails. Returns false when it cannot start, such as while stlxdm holds gfxfb's buffers
bool start_presenting(void);

// Whether start_presenting() succeeded and stop_presenting() has not run since
bool is_presenting(void);

// How many presents ended so far, on time or not
uint64_t presents_ended(void);

// Stops presenting. Returns whether presents ended, each latching within GFXFB_PRESENT_TIMEOUT_NS of its call's return
bool stop_presenting(void);

// Creates and destroys gfxfb's buffers of the primary output's size in stlxdm's place, from a thread of its own, until
// stop_cycling_buffers() or until a call fails, which it prints. Returns false, printing why, when it cannot start
bool start_cycling_buffers(void);

// How many buffers the cycling has created and destroyed so far
uint64_t cycled_buffers(void);

// Stops the cycling and closes /dev/gfxfb once its thread finishes the buffer in progress. Must follow a start that
// returned true. Returns false when a failed call ended the cycling early
bool stop_cycling_buffers(void);

#define HELD_CHILD_ARG "held-child"

// The child a window check kills, run as `gputest held-child`: it shows a GPU picture, leaves another pending, writes a
// byte to handle 3 and sleeps. Returns 1 if it could not
int run_held_child(void);

#endif
