/*
 * Intrusive red-black tree.
 *
 * A type-safe, intrusive RB-tree for freestanding kernel use. Nodes are
 * embedded directly in user structs via rbt::node fields, so no allocation
 * is needed for tree operations. A single struct can participate in multiple
 * trees by embedding multiple rbt::node fields.
 *
 * Usage:
 *   struct my_item {
 *       uint64_t key;
 *       rbt::node tree_link;
 *   };
 *
 *   struct my_compare {
 *       bool operator()(const my_item& a, const my_item& b) const {
 *           return a.key < b.key;
 *       }
 *   };
 *
 *   rbt::tree<my_item, &my_item::tree_link, my_compare> my_tree;
 *   my_tree.insert(&item);
 *   my_tree.remove(item);
 *   my_item* found = my_tree.find(probe);
 *
 * Thread safety: none. Caller must synchronize concurrent access.
 */

#ifndef STELLUX_CORE_DS_RB_TREE_H
#define STELLUX_CORE_DS_RB_TREE_H

#include "core/types.h"

namespace rbt {

enum class color : uint8_t {
    RED,
    BLACK
};

// Intrusive tree node. Embed in your struct to make it tree-insertable.
// Default-initialized to safe values so forgetting to init won't corrupt.
struct node {
    node* parent = nullptr;
    node* left   = nullptr;
    node* right  = nullptr;
    color col    = color::RED;
};

// Low-level core operations on raw node pointers.
// These are implemented in rb_tree.cpp (compiled once, not templated).

// Rebalance after BST insertion. Caller must have already linked the
// red node into the tree at the correct BST position.
void insert_fixup(node** root, node* n);

// Remove a node from the tree and rebalance.
void remove_node(node** root, node* n);

// In-order successor (nullptr if n is the maximum).
node* next(const node* n);

// In-order predecessor (nullptr if n is the minimum).
node* prev(const node* n);

// Leftmost descendant of subtree (nullptr if subtree is nullptr).
node* minimum(node* subtree);

// Rightmost descendant of subtree (nullptr if subtree is nullptr).
node* maximum(node* subtree);

// Validate all RB invariants. Returns false on violation.
[[nodiscard]] bool validate(const node* root, size_t expected_count);
[[nodiscard]] bool validate(const node* root, size_t expected_count,
                            const char*& err_out);

// Convert an rbt::node pointer back to the containing struct.
// Returns nullptr if n is nullptr.
template <typename T, node T::*Member>
inline T* node_to_entry(node* n) {
    if (!n) return nullptr;
    // Standard container_of: compute offset of Member within T, subtract from n
    const uintptr_t offset = reinterpret_cast<uintptr_t>(
        &(static_cast<T*>(nullptr)->*Member));
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(n) - offset);
}

template <typename T, node T::*Member>
inline const T* node_to_entry(const node* n) {
    if (!n) return nullptr;
    const uintptr_t offset = reinterpret_cast<uintptr_t>(
        &(static_cast<const T*>(nullptr)->*Member));
    return reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(n) - offset);
}

// Type-safe intrusive red-black tree.
// T      — the containing struct type
// Link   — pointer-to-member identifying which rbt::node field to use
// Compare — functor with bool operator()(const T&, const T&) for less-than
template <typename T, node T::*Link, typename Compare>
class tree {
public:
    constexpr tree() : root_(nullptr), count_(0), cmp_() {}
    explicit constexpr tree(Compare cmp) : root_(nullptr), count_(0), cmp_(cmp) {}

    [[nodiscard]] constexpr bool empty() const { return root_ == nullptr; }
    [[nodiscard]] constexpr size_t size() const { return count_; }

    // Return the minimum/maximum entry, or nullptr if tree is empty.
    [[nodiscard]] T* min() const {
        return to_entry(minimum(root_));
    }

    [[nodiscard]] T* max() const {
        return to_entry(maximum(root_));
    }

    // Insert entry into the tree. Returns true if inserted, false if a
    // duplicate (according to Compare) already exists.
    [[nodiscard]] bool insert(T* entry) {
        node* n = to_node(entry);
        n->left = nullptr;
        n->right = nullptr;
        n->col = color::RED;

        // BST descent to find insertion point
        node* parent = nullptr;
        node* cur = root_;
        while (cur) {
            parent = cur;
            const T* cur_entry = to_entry(cur);
            if (cmp_(*entry, *cur_entry)) {
                cur = cur->left;
            } else if (cmp_(*cur_entry, *entry)) {
                cur = cur->right;
            } else {
                return false; // duplicate
            }
        }

        n->parent = parent;
        if (!parent) {
            root_ = n;
        } else if (cmp_(*entry, *to_entry(parent))) {
            parent->left = n;
        } else {
            parent->right = n;
        }

        insert_fixup(&root_, n);
        ++count_;
        return true;
    }

    // Remove entry from this tree. Entry must be a member of this tree.
    void remove(T& entry) {
        node* n = to_node(entry);
        remove_node(&root_, n);
        --count_;
        // Clear removed node's links for safety
        n->parent = nullptr;
        n->left = nullptr;
        n->right = nullptr;
    }

    // Find an entry matching probe. Returns nullptr if not found.
    [[nodiscard]] T* find(const T& probe) const {
        node* cur = root_;
        while (cur) {
            const T* cur_entry = to_entry(cur);
            if (cmp_(probe, *cur_entry)) {
                cur = cur->left;
            } else if (cmp_(*cur_entry, probe)) {
                cur = cur->right;
            } else {
                return const_cast<T*>(cur_entry);
            }
        }
        return nullptr;
    }

    // Return the first entry >= probe, or nullptr if none.
    [[nodiscard]] T* lower_bound(const T& probe) const {
        node* cur = root_;
        node* candidate = nullptr;
        while (cur) {
            const T* cur_entry = to_entry(cur);
            if (cmp_(*cur_entry, probe)) {
                cur = cur->right;
            } else {
                candidate = cur;
                cur = cur->left;
            }
        }
        return to_entry(candidate);
    }

    // Return the first entry > probe, or nullptr if none.
    [[nodiscard]] T* upper_bound(const T& probe) const {
        node* cur = root_;
        node* candidate = nullptr;
        while (cur) {
            const T* cur_entry = to_entry(cur);
            if (cmp_(probe, *cur_entry)) {
                candidate = cur;
                cur = cur->left;
            } else {
                cur = cur->right;
            }
        }
        return to_entry(candidate);
    }

    // In-order successor of entry. Returns nullptr if entry is the max.
    [[nodiscard]] T* next(const T& entry) const {
        const node* n = to_node(entry);
        return to_entry(rbt::next(n));
    }

    // In-order predecessor of entry. Returns nullptr if entry is the min.
    [[nodiscard]] T* prev(const T& entry) const {
        const node* n = to_node(entry);
        return to_entry(rbt::prev(n));
    }

    [[nodiscard]] bool validate() const {
        return rbt::validate(root_, count_);
    }

    [[nodiscard]] bool validate(const char*& err_out) const {
        return rbt::validate(root_, count_, err_out);
    }

    // Forward iterator for in-order traversal.
    class iterator {
    public:
        constexpr iterator() : cur_(nullptr) {}
        explicit constexpr iterator(node* n) : cur_(n) {}

        T& operator*() const { return *node_to_entry<T, Link>(cur_); }
        T* operator->() const { return node_to_entry<T, Link>(cur_); }

        iterator& operator++() {
            cur_ = rbt::next(cur_);
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            cur_ = rbt::next(cur_);
            return tmp;
        }

        bool operator==(const iterator& other) const { return cur_ == other.cur_; }
        bool operator!=(const iterator& other) const { return cur_ != other.cur_; }

    private:
        node* cur_;
    };

    iterator begin() const { return iterator(minimum(root_)); }
    iterator end() const { return iterator(nullptr); }

private:
    node* root_;
    size_t count_;
    Compare cmp_;

    static constexpr node* to_node(T* e) { return &(e->*Link); }
    static constexpr node* to_node(T& e) { return &(e.*Link); }
    static constexpr const node* to_node(const T& e) { return &(e.*Link); }

    static T* to_entry(node* n) {
        return node_to_entry<T, Link>(n);
    }

    static const T* to_entry(const node* n) {
        return node_to_entry<T, Link>(const_cast<node*>(n));
    }
};

} // namespace rbt

#endif // STELLUX_CORE_DS_RB_TREE_H
