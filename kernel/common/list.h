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
        sentinel_.prev = &sentinel_;
        sentinel_.next = &sentinel_;
        count_ = 0;
    }

    [[nodiscard]] bool empty() const { return sentinel_.next == &sentinel_; }
    [[nodiscard]] size_t size() const { return count_; }

    void push_back(T* item) {
        node* n = to_node(item);
        n->prev = sentinel_.prev;
        n->next = &sentinel_;
        sentinel_.prev->next = n;
        sentinel_.prev = n;
        ++count_;
    }

    void push_front(T* item) {
        node* n = to_node(item);
        n->prev = &sentinel_;
        n->next = sentinel_.next;
        sentinel_.next->prev = n;
        sentinel_.next = n;
        ++count_;
    }

    T* pop_front() {
        if (empty()) return nullptr;
        node* n = sentinel_.next;
        unlink(n);
        --count_;
        return to_entry(n);
    }

    T* pop_back() {
        if (empty()) return nullptr;
        node* n = sentinel_.prev;
        unlink(n);
        --count_;
        return to_entry(n);
    }

    // Remove a specific item. Item must be in this list.
    void remove(T* item) {
        node* n = to_node(item);
        unlink(n);
        --count_;
    }

    [[nodiscard]] T* front() {
        if (empty()) return nullptr;
        return to_entry(sentinel_.next);
    }

    [[nodiscard]] T* back() {
        if (empty()) return nullptr;
        return to_entry(sentinel_.prev);
    }

    [[nodiscard]] const T* front() const {
        if (empty()) return nullptr;
        return node_to_entry<T, Link>(sentinel_.next);
    }

    [[nodiscard]] const T* back() const {
        if (empty()) return nullptr;
        return node_to_entry<T, Link>(sentinel_.prev);
    }

    // Forward iterator for traversal.
    class iterator {
    public:
        explicit constexpr iterator(node* n, const node* sentinel)
            : cur_(n), sentinel_(sentinel) {}

        T& operator*() const { return *node_to_entry<T, Link>(cur_); }
        T* operator->() const { return node_to_entry<T, Link>(cur_); }

        iterator& operator++() {
            cur_ = cur_->next;
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            cur_ = cur_->next;
            return tmp;
        }

        bool operator==(const iterator& other) const { return cur_ == other.cur_; }
        bool operator!=(const iterator& other) const { return cur_ != other.cur_; }

    private:
        node* cur_;
        const node* sentinel_;
    };

    iterator begin() { return iterator(sentinel_.next, &sentinel_); }
    iterator end() { return iterator(&sentinel_, &sentinel_); }

    /**
     * Insert item in sorted order. Pred(a, b) returns true if a should
     * come before b. The item is inserted before the first element for
     * which before(item, existing) is true. If no such element, appends.
     */
    template<typename Pred>
    void insert_sorted(T* item, Pred before) {
        node* n = to_node(item);
        node* cur = sentinel_.next;
        while (cur != &sentinel_) {
            if (before(item, to_entry(cur))) {
                n->prev = cur->prev;
                n->next = cur;
                cur->prev->next = n;
                cur->prev = n;
                ++count_;
                return;
            }
            cur = cur->next;
        }
        push_back(item);
    }

private:
    node   sentinel_;
    size_t count_ = 0;

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
