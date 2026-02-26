/*
 * Intrusive hash map.
 *
 * A type-safe, intrusive hash map for freestanding kernel use. Nodes are
 * embedded directly in user structs via hashmap::node fields, so insert
 * and remove never allocate. The caller provides the bucket array.
 *
 * Uses Linux's hlist pprev trick: each node stores a pointer-to-pointer
 * (pprev) that points to the pointer referencing it, enabling O(1)
 * removal without knowing the bucket head.
 *
 * Usage:
 *   struct my_item {
 *       uint64_t id;
 *       hashmap::node hash_link;
 *   };
 *
 *   struct my_key_ops {
 *       using key_type = uint64_t;
 *       static key_type key_of(const my_item& e) { return e.id; }
 *       static uint64_t hash(const key_type& k) { return hash::u64(k); }
 *       static bool equal(const key_type& a, const key_type& b) { return a == b; }
 *   };
 *
 *   hashmap::bucket buckets[64] = {};
 *   hashmap::map<my_item, &my_item::hash_link, my_key_ops> table;
 *   table.init(buckets, 64);
 *   table.insert(&item);
 *   my_item* found = table.find(42);
 *
 * Thread safety: none. Caller must synchronize concurrent access.
 */

#ifndef STELLUX_COMMON_HASHMAP_H
#define STELLUX_COMMON_HASHMAP_H

#include "types.h"

namespace hashmap {

// Intrusive node. Embed in your struct.
// pprev points to the pointer that references this node
// (&bucket.first or &prev->next), enabling O(1) uniform deletion.
// *Note* this optimization is borrowed from Linux's hlist implementation.
struct node {
    node*  next  = nullptr;
    node** pprev = nullptr;
};

// Bucket head. Single pointer -- half the memory of a doubly-linked head.
struct bucket {
    node* first = nullptr;
};

// Convert a hashmap::node pointer back to the containing struct.
template<typename T, node T::*Member>
inline T* node_to_entry(node* n) {
    if (!n) return nullptr;
    const uintptr_t offset = reinterpret_cast<uintptr_t>(
        &(static_cast<T*>(nullptr)->*Member));
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(n) - offset);
}

template<typename T, node T::*Member>
inline const T* node_to_entry(const node* n) {
    if (!n) return nullptr;
    const uintptr_t offset = reinterpret_cast<uintptr_t>(
        &(static_cast<const T*>(nullptr)->*Member));
    return reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(n) - offset);
}

// Type-safe intrusive hash map.
// T       — the containing struct type
// Link    — pointer-to-member identifying which hashmap::node field to use
// KeyOps  — traits struct providing:
//             using key_type = ...;
//             static key_type key_of(const T&);
//             static uint64_t hash(const key_type&);
//             static bool equal(const key_type&, const key_type&);
template<typename T, node T::*Link, typename KeyOps>
class map {
public:
    using key_type = typename KeyOps::key_type;

    constexpr map() : m_buckets(nullptr), m_mask(0), m_count(0) {}

    // Initialize with caller-provided bucket array.
    // bucket_count must be a power of 2 and > 0.
    // Buckets must be zero-initialized.
    void init(bucket* buckets, uint32_t bucket_count) {
        if (!buckets || bucket_count == 0 ||
            (bucket_count & (bucket_count - 1)) != 0) {
            bad_init(buckets, bucket_count);
        }
        m_buckets = buckets;
        m_mask = bucket_count - 1;
        m_count = 0;
        for (uint32_t i = 0; i < bucket_count; ++i)
            m_buckets[i].first = nullptr;
    }

    // Insert entry at head of its bucket chain. Infallible.
    // Does NOT check for duplicates.
    void insert(T* entry) {
        if (!m_buckets) return;
        node* n = to_node(entry);
        uint64_t h = KeyOps::hash(KeyOps::key_of(*entry));
        uint32_t idx = static_cast<uint32_t>(h) & m_mask;
        bucket& b = m_buckets[idx];

        n->next = b.first;
        n->pprev = &b.first;
        if (b.first) b.first->pprev = &n->next;
        b.first = n;
        ++m_count;
    }

    // Remove entry from the map. O(1) via pprev trick.
    // Entry must be in this map (pprev != nullptr).
    void remove(T& entry) {
        node* n = to_node(entry);
        *n->pprev = n->next;
        if (n->next) n->next->pprev = n->pprev;
        n->next = nullptr;
        n->pprev = nullptr;
        --m_count;
    }

    // Unlink all entries without freeing (entries are caller-owned).
    void clear() {
        if (!m_buckets) return;
        for (uint32_t i = 0; i <= m_mask; ++i) {
            node* cur = m_buckets[i].first;
            while (cur) {
                node* nxt = cur->next;
                cur->next = nullptr;
                cur->pprev = nullptr;
                cur = nxt;
            }
            m_buckets[i].first = nullptr;
        }
        m_count = 0;
    }

    // Find first entry matching key. Returns nullptr if not found.
    [[nodiscard]] T* find(const key_type& key) const {
        if (!m_buckets) return nullptr;
        uint64_t h = KeyOps::hash(key);
        uint32_t idx = static_cast<uint32_t>(h) & m_mask;
        node* cur = m_buckets[idx].first;
        while (cur) {
            T* entry = to_entry(cur);
            if (KeyOps::equal(KeyOps::key_of(*entry), key)) {
                return entry;
            }
            cur = cur->next;
        }
        return nullptr;
    }

    [[nodiscard]] bool empty() const { return m_count == 0; }
    [[nodiscard]] uint32_t size() const { return m_count; }
    [[nodiscard]] uint32_t bucket_count() const { return m_mask + 1; }

    // Iterate all entries. Safe to call remove() on the current entry
    // inside fn — next pointer is captured before each callback.
    template<typename Fn>
    void for_each(Fn fn) {
        if (!m_buckets) return;
        for (uint32_t i = 0; i <= m_mask; ++i) {
            node* cur = m_buckets[i].first;
            while (cur) {
                node* nxt = cur->next;
                fn(*to_entry(cur));
                cur = nxt;
            }
        }
    }

    // Iterate entries in the bucket for the given key. Safe for removal.
    template<typename Fn>
    void for_each_possible(const key_type& key, Fn fn) {
        if (!m_buckets) return;
        uint64_t h = KeyOps::hash(key);
        uint32_t idx = static_cast<uint32_t>(h) & m_mask;
        node* cur = m_buckets[idx].first;
        while (cur) {
            node* nxt = cur->next;
            fn(*to_entry(cur));
            cur = nxt;
        }
    }

private:
    bucket*  m_buckets;
    uint32_t m_mask;
    uint32_t m_count;

    static constexpr node* to_node(T* e) { return &(e->*Link); }
    static constexpr node* to_node(T& e) { return &(e.*Link); }

    static T* to_entry(node* n) {
        return node_to_entry<T, Link>(n);
    }

    // Separate function so the cold path doesn't inline into init().
    [[noreturn]] static void bad_init(const bucket* b, uint32_t c);
};

} // namespace hashmap

// The fatal handler is defined out-of-class to keep logging.h out of
// this header. It must be compiled exactly once (ODR), so we rely on
// the linker to deduplicate this weak definition across translation units.
#include "logging.h"

template<typename T, hashmap::node T::*Link, typename KeyOps>
[[noreturn]] void hashmap::map<T, Link, KeyOps>::bad_init(
    const hashmap::bucket* b, uint32_t c) {
    log::fatal("hashmap::init: invalid args (buckets=%p, count=%u)",
               b, c);
}

#endif // STELLUX_COMMON_HASHMAP_H
