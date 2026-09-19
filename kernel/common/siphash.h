#ifndef STELLUX_COMMON_SIPHASH_H
#define STELLUX_COMMON_SIPHASH_H

#include "common/types.h"

namespace hash {

/**
 * The 128-bit secret a SipHash value depends on, drawn once from the random
 * source by the owner of a table or a sequence space and never from data.
 */
struct siphash_key {
    uint64_t k0;
    uint64_t k1;
};

/**
 * @brief SipHash-2-4 of `len` bytes under `key`. Without the key the output
 * cannot be predicted, so a table or a sequence space keyed with it cannot be
 * steered by a peer choosing its inputs.
 */
uint64_t siphash(const void* data, size_t len, const siphash_key& key);

} // namespace hash

#endif // STELLUX_COMMON_SIPHASH_H
