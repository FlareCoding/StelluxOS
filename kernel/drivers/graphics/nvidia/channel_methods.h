// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHANNEL_METHODS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHANNEL_METHODS_H

#include "common/types.h"

/*
 * The words the GPU fetches for a channel, and the values that allocate a
 * channel and ring its doorbell. A GPFIFO entry points the GPU at method words
 * (an engine's commands, each a header and its values) in the channel's
 * address space. A release sequence writes a semaphore (a word in memory the
 * GPU writes when work completes), then raises the channel's nonstall
 * interrupt (one that notifies the driver without stalling the engine).
 */

namespace drivers::nvidia {

// NVIDIA's NVC56F_GP_ENTRY__SIZE, the bytes of a GPFIFO entry
constexpr uint32_t GPFIFO_ENTRY_BYTES = 8;

// The most method words a GPFIFO entry runs, the largest count NVIDIA's NVC56F_GP_ENTRY1_LENGTH field holds
constexpr uint32_t GPFIFO_ENTRY_WORDS_MAX = (1u << 21) - 1;

// The most bytes one fill writes, as the copy engine counts a fill's 4-byte elements in 32 bits
constexpr uint64_t FILL_BYTES_MAX = 4ull * 0xFFFFFFFFull;

// The end of the video memory a fill reaches, as NVIDIA's NVC7B5_OFFSET_OUT_UPPER holds address bits 48:32
constexpr uint64_t FILL_END_MAX = 1ull << 49;

// The method words `write_fill` and `write_release` write
constexpr uint32_t FILL_WORDS    = 15;
constexpr uint32_t RELEASE_WORDS = 20;

struct gpfifo_entry {
    uint32_t words[2];
};

// When the GPU's host may fetch an entry's method words. Processing an entry's methods does not wait for the engine
// work they start, unless they wait for idle
enum class gpfifo_fetch : uint8_t {
    ahead,         // while the host still processes the entries before it
    after_earlier, // once the host processed the entries before it
};

// Which pipes of the engine (a graphics engine has a 3D and a compute pipe) a release's wait for idle covers, as
// NVIDIA's NVC56F_WFI_SCOPE values say
enum class idle_scope : uint32_t {
    current_pipe = 0, // NVC56F_WFI_SCOPE_CURRENT_SCG_TYPE, the work of the type the engine runs now
    every_pipe = 1,   // NVC56F_WFI_SCOPE_ALL, a graphics engine's 3D and compute work alike
};

/**
 * @brief The GPFIFO entry running the `word_count` method words at
 * `gpu_address` in the channel's address space, fetched as `fetch` says.
 * @return The entry, as the GPU reads it from the GPFIFO.
 * @note `gpu_address` must be 4-byte aligned and below 2^40, and `word_count`
 * at most `GPFIFO_ENTRY_WORDS_MAX`, since the entry has no bits for more.
 */
gpfifo_entry make_gpfifo_entry(uint64_t gpu_address, uint32_t word_count, gpfifo_fetch fetch);

/**
 * @brief Write to `words` the `FILL_WORDS` method words that bind `copy_class`
 * and have the channel's copy engine write `value` to every 4-byte element of
 * the `bytes` of video memory at `offset`.
 * @note `offset` and `bytes` must be multiples of 4, `bytes` nonzero and at
 * most `FILL_BYTES_MAX`, and the range must end by `FILL_END_MAX`. Only a
 * privileged channel's copy engine may write by physical address.
 */
void write_fill(uint32_t* words, uint32_t copy_class, uint64_t offset, uint64_t bytes, uint32_t value);

/**
 * @brief Write to `words` the `RELEASE_WORDS` method words of a release
 * sequence: wait for the engine to idle over `scope`, make earlier writes
 * visible, write `payload` to the 4-byte semaphore at `semaphore_address` in
 * the channel's address space, make it visible, then raise the channel's
 * nonstall interrupt.
 * @note `semaphore_address` must be 4-byte aligned and below 2^40, since the
 * method has no bits for more.
 */
void write_release(uint32_t* words, uint64_t semaphore_address, uint32_t payload, idle_scope scope);

// The token whose write to the doorbell register has the GPU fetch new entries of channel `channel_id` on `runlist`
uint32_t doorbell_token(uint32_t runlist, uint32_t channel_id);

/**
 * @brief The flags of a channel's allocation: its id pinned to `channel_id`,
 * scheduling left to an explicit schedule control, and when `is_privileged`
 * its copy engine allowed physical addresses, else the channel denied them
 * and the privileged MEM_OP operations.
 * @return The flags, as NVIDIA's NVOS04 flags lay them out.
 * @note `channel_id` must be below 4096, the ids the flags' 9-bit USERD page
 * and 3-bit index can name.
 */
uint32_t channel_allocation_flags(uint32_t channel_id, bool is_privileged);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CHANNEL_METHODS_H
