/*
 * Intrusive doubly-linked circular list.
 *
 * A type-safe, intrusive list for freestanding kernel use. Nodes are
 * embedded directly in user structs via list::node fields, so no allocation
 * is needed for list operations.
 *
 * Usage:
 *   struct my_item {
 *       uint64_t value;
 *       list::node link;
 *   };
 *
 *   list::head<my_item, &my_item::link> my_list;
 *   my_list.init();
 *   my_list.push_back(&item);
 *   my_item* front = my_list.pop_front();
 *
 * Thread safety: none. Caller must synchronize concurrent access.
 */

#ifndef STELLUX_COMMON_LIST_H
#define STELLUX_COMMON_LIST_H

#include "types.h"

namespace list {

struct node {
    node* prev = nullptr;
    node* next = nullptr;
    bool is_linked() const { return prev != nullptr && next != nullptr; }
};

// Convert a list::node pointer back to the containing struct.
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

// Type-safe intrusive doubly-linked circular list.
// T    -- the containing struct type
// Link -- pointer-to-member identifying which list::node field to use
template<typename T, node T::*Link>
class head {
public:
    void init() {
        m_sentinel.prev = &m_sentinel;
        m_sentinel.next = &m_sentinel;
        m_count = 0;
    }

    [[nodiscard]] bool empty() const { return m_sentinel.next == &m_sentinel; }
    [[nodiscard]] size_t size() const { return m_count; }

    void push_back(T* item) {
        node* n = to_node(item);
        n->prev = m_sentinel.prev;
        n->next = &m_sentinel;
        m_sentinel.prev->next = n;
        m_sentinel.prev = n;
        ++m_count;
    }

    void push_front(T* item) {
        node* n = to_node(item);
        n->prev = &m_sentinel;
        n->next = m_sentinel.next;
        m_sentinel.next->prev = n;
        m_sentinel.next = n;
        ++m_count;
    }

    T* pop_front() {
        if (empty()) return nullptr;
        node* n = m_sentinel.next;
        unlink(n);
        --m_count;
        return to_entry(n);
    }

    T* pop_back() {
        if (empty()) return nullptr;
        node* n = m_sentinel.prev;
        unlink(n);
        --m_count;
        return to_entry(n);
    }

    // Remove a specific item. Item must be in this list.
    void remove(T* item) {
        node* n = to_node(item);
        unlink(n);
        --m_count;
    }

    [[nodiscard]] T* front() {
        if (empty()) return nullptr;
        return to_entry(m_sentinel.next);
    }

    [[nodiscard]] T* back() {
        if (empty()) return nullptr;
        return to_entry(m_sentinel.prev);
    }

    [[nodiscard]] const T* front() const {
        if (empty()) return nullptr;
        return node_to_entry<T, Link>(m_sentinel.next);
    }

    [[nodiscard]] const T* back() const {
        if (empty()) return nullptr;
        return node_to_entry<T, Link>(m_sentinel.prev);
    }

    // Forward iterator for traversal.
    class iterator {
    public:
        explicit constexpr iterator(node* n, const node* sentinel)
            : m_cur(n), m_sentinel(sentinel) {}

        T& operator*() const { return *node_to_entry<T, Link>(m_cur); }
        T* operator->() const { return node_to_entry<T, Link>(m_cur); }

        iterator& operator++() {
            m_cur = m_cur->next;
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            m_cur = m_cur->next;
            return tmp;
        }

        bool operator==(const iterator& other) const { return m_cur == other.m_cur; }
        bool operator!=(const iterator& other) const { return m_cur != other.m_cur; }

    private:
        node* m_cur;
        const node* m_sentinel;
    };

    iterator begin() { return iterator(m_sentinel.next, &m_sentinel); }
    iterator end() { return iterator(&m_sentinel, &m_sentinel); }

    /**
     * Insert item in sorted order. Pred(a, b) returns true if a should
     * come before b. The item is inserted before the first element for
     * which before(item, existing) is true. If no such element, appends.
     */
    template<typename Pred>
    void insert_sorted(T* item, Pred before) {
        node* n = to_node(item);
        node* cur = m_sentinel.next;
        while (cur != &m_sentinel) {
            if (before(item, to_entry(cur))) {
                n->prev = cur->prev;
                n->next = cur;
                cur->prev->next = n;
                cur->prev = n;
                ++m_count;
                return;
            }
            cur = cur->next;
        }
        push_back(item);
    }

private:
    node   m_sentinel;
    size_t m_count = 0;

    static node* to_node(T* e) { return &(e->*Link); }

    static T* to_entry(node* n) {
        return node_to_entry<T, Link>(n);
    }

    static void unlink(node* n) {
        n->prev->next = n->next;
        n->next->prev = n->prev;
        n->prev = nullptr;
        n->next = nullptr;
    }
};

} // namespace list

#endif // STELLUX_COMMON_LIST_H
