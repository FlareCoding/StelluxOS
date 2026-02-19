/*
 * Intrusive red-black tree — core algorithms.
 *
 * All functions operate on raw rbt::node pointers and are compiled once.
 * The template wrapper in rb_tree.h calls into these.
 *
 * Algorithms follow CLRS (Introduction to Algorithms) adapted for:
 *   - nullptr leaves instead of a sentinel node
 *   - Intrusive node swap instead of key copy for deletion with two children
 */

#include "common/ds/rb_tree.h"

namespace rbt {

// Rotate node x to the left. x's right child moves up.
//
//     x            y
//    / \          / \
//   a   y   =>  x   c
//      / \     / \
//     b   c   a   b
//
static void rotate_left(node** root, node* x) {
    node* y = x->right;
    x->right = y->left;
    if (y->left) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (!x->parent) {
        *root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

// Rotate node x to the right. x's left child moves up.
//
//       x        y
//      / \      / \
//     y   c => a   x
//    / \          / \
//   a   b        b   c
//
static void rotate_right(node** root, node* x) {
    node* y = x->left;
    x->left = y->right;
    if (y->right) {
        y->right->parent = x;
    }
    y->parent = x->parent;
    if (!x->parent) {
        *root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->right = x;
    x->parent = y;
}

static inline bool is_red(const node* n) {
    return n && n->col == color::RED;
}

static inline bool is_black(const node* n) {
    return !n || n->col == color::BLACK;
}

// Rebalance after insertion (CLRS INSERT-FIXUP).
// n must be a red node that was just linked into the tree.
void insert_fixup(node** root, node* n) {
    while (n->parent && n->parent->col == color::RED) {
        node* p = n->parent;
        node* g = p->parent;

        if (p == g->left) {
            node* u = g->right; // uncle
            if (is_red(u)) {
                // Case 1: uncle is red — recolor and move up
                p->col = color::BLACK;
                u->col = color::BLACK;
                g->col = color::RED;
                n = g;
            } else {
                if (n == p->right) {
                    // Case 2: uncle black, n is inner child — rotate to outer
                    n = p;
                    rotate_left(root, n);
                    p = n->parent;
                    g = p->parent;
                }
                // Case 3: uncle black, n is outer child — rotate + recolor
                p->col = color::BLACK;
                g->col = color::RED;
                rotate_right(root, g);
            }
        } else {
            // Mirror: p is right child of g
            node* u = g->left; // uncle
            if (is_red(u)) {
                p->col = color::BLACK;
                u->col = color::BLACK;
                g->col = color::RED;
                n = g;
            } else {
                if (n == p->left) {
                    n = p;
                    rotate_right(root, n);
                    p = n->parent;
                    g = p->parent;
                }
                p->col = color::BLACK;
                g->col = color::RED;
                rotate_left(root, g);
            }
        }
    }
    (*root)->col = color::BLACK;
}

// Replace subtree rooted at u with subtree rooted at v.
// v may be nullptr (removing a leaf).
static void transplant(node** root, node* u, node* v) {
    if (!u->parent) {
        *root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    if (v) {
        v->parent = u->parent;
    }
}

// Fix double-black after deletion (CLRS DELETE-FIXUP).
// x is the node that replaced the removed node (may be nullptr).
// x_parent is x's parent (needed when x is nullptr).
static void delete_fixup(node** root, node* x, node* x_parent) {
    while (x != *root && is_black(x)) {
        if (x == x_parent->left) {
            node* w = x_parent->right; // sibling

            // Case 1: sibling is red
            if (is_red(w)) {
                w->col = color::BLACK;
                x_parent->col = color::RED;
                rotate_left(root, x_parent);
                w = x_parent->right;
            }

            // Case 2: sibling black, both children black
            if (is_black(w->left) && is_black(w->right)) {
                w->col = color::RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                // Case 3: sibling black, near child red, far child black
                if (is_black(w->right)) {
                    if (w->left) w->left->col = color::BLACK;
                    w->col = color::RED;
                    rotate_right(root, w);
                    w = x_parent->right;
                }
                // Case 4: sibling black, far child red
                w->col = x_parent->col;
                x_parent->col = color::BLACK;
                if (w->right) w->right->col = color::BLACK;
                rotate_left(root, x_parent);
                x = *root; // terminate loop
            }
        } else {
            // Mirror: x is right child
            node* w = x_parent->left;

            if (is_red(w)) {
                w->col = color::BLACK;
                x_parent->col = color::RED;
                rotate_right(root, x_parent);
                w = x_parent->left;
            }

            if (is_black(w->right) && is_black(w->left)) {
                w->col = color::RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (is_black(w->left)) {
                    if (w->right) w->right->col = color::BLACK;
                    w->col = color::RED;
                    rotate_left(root, w);
                    w = x_parent->left;
                }
                w->col = x_parent->col;
                x_parent->col = color::BLACK;
                if (w->left) w->left->col = color::BLACK;
                rotate_right(root, x_parent);
                x = *root;
            }
        }
    }
    if (x) x->col = color::BLACK;
}

// Physically swap two nodes' positions in the tree (not their data).
// Required for intrusive deletion when the node has two children:
// we can't copy the successor's key, so we swap tree positions instead.
static void swap_nodes(node** root, node* a, node* b) {
    // Save all links
    node* a_parent = a->parent;
    node* a_left = a->left;
    node* a_right = a->right;
    color a_col = a->col;

    node* b_parent = b->parent;
    node* b_left = b->left;
    node* b_right = b->right;
    color b_col = b->col;

    bool a_is_left = a_parent && (a == a_parent->left);
    bool b_is_left = b_parent && (b == b_parent->left);

    // Handle the case where a and b are parent-child
    if (a->parent == b) {
        // b is parent of a — swap so a is always the parent
        swap_nodes(root, b, a);
        return;
    }

    if (b->parent == a) {
        // a is parent of b
        bool b_is_left_of_a = (b == a->left);

        // Update a's parent to point to b
        if (!a_parent) {
            *root = b;
        } else if (a_is_left) {
            a_parent->left = b;
        } else {
            a_parent->right = b;
        }
        b->parent = a_parent;

        // Wire b's children
        if (b_is_left_of_a) {
            b->left = a;
            b->right = a_right;
            if (a_right) a_right->parent = b;
        } else {
            b->right = a;
            b->left = a_left;
            if (a_left) a_left->parent = b;
        }
        a->parent = b;

        // Wire a's children (previously b's children)
        a->left = b_left;
        a->right = b_right;
        if (b_left) b_left->parent = a;
        if (b_right) b_right->parent = a;
    } else {
        // a and b are not adjacent — general case

        // Update parents to point to the swapped node
        if (!a_parent) {
            *root = b;
        } else if (a_is_left) {
            a_parent->left = b;
        } else {
            a_parent->right = b;
        }

        if (!b_parent) {
            *root = a;
        } else if (b_is_left) {
            b_parent->left = a;
        } else {
            b_parent->right = a;
        }

        // Swap parent pointers
        b->parent = a_parent;
        a->parent = b_parent;

        // Swap children
        b->left = a_left;
        b->right = a_right;
        a->left = b_left;
        a->right = b_right;

        // Update children's parent pointers
        if (a_left) a_left->parent = b;
        if (a_right) a_right->parent = b;
        if (b_left) b_left->parent = a;
        if (b_right) b_right->parent = a;
    }

    // Swap colors
    a->col = b_col;
    b->col = a_col;
}

// Remove node n from the tree and rebalance.
void remove_node(node** root, node* n) {
    // If n has two children, swap with in-order successor first
    if (n->left && n->right) {
        node* successor = minimum(n->right);
        swap_nodes(root, n, successor);
        // n is now in the successor's old position (has at most one child)
    }

    // n now has at most one child
    node* child = n->left ? n->left : n->right;
    node* parent = n->parent;
    color removed_color = n->col;

    transplant(root, n, child);

    // Fix up if we removed a black node
    if (removed_color == color::BLACK) {
        if (child) {
            // child exists — fixup from child
            delete_fixup(root, child, child->parent);
        } else if (parent) {
            // child is nullptr (double-black nil) — fixup from parent
            delete_fixup(root, nullptr, parent);
        }
        // else: removed the last node, tree is empty — nothing to fix
    }
}

// In-order successor: smallest node greater than n.
node* next(const node* n) {
    if (!n) return nullptr;

    // If right subtree exists, successor is its minimum
    if (n->right) {
        return minimum(n->right);
    }

    // Otherwise, walk up until we find an ancestor where n is in left subtree
    node* p = n->parent;
    while (p && n == p->right) {
        n = p;
        p = p->parent;
    }
    return p;
}

// In-order predecessor: largest node less than n.
node* prev(const node* n) {
    if (!n) return nullptr;

    if (n->left) {
        return maximum(n->left);
    }

    node* p = n->parent;
    while (p && n == p->left) {
        n = p;
        p = p->parent;
    }
    return p;
}

// Leftmost descendant of subtree.
node* minimum(node* subtree) {
    if (!subtree) return nullptr;
    while (subtree->left) {
        subtree = subtree->left;
    }
    return subtree;
}

// Rightmost descendant of subtree.
node* maximum(node* subtree) {
    if (!subtree) return nullptr;
    while (subtree->right) {
        subtree = subtree->right;
    }
    return subtree;
}

// Recursive validation helper. Returns black height, or -1 on error.
static int32_t validate_subtree(const node* n, const node* parent,
                                size_t& count, const char*& err) {
    if (!n) return 0; // nullptr leaves are black, height 0

    // Parent consistency
    if (n->parent != parent) {
        err = "parent pointer inconsistent";
        return -1;
    }

    // Red-red violation
    if (n->col == color::RED) {
        if (is_red(n->left) || is_red(n->right)) {
            err = "red node has red child";
            return -1;
        }
    }

    ++count;

    int32_t lh = validate_subtree(n->left, n, count, err);
    if (lh < 0) return -1;

    int32_t rh = validate_subtree(n->right, n, count, err);
    if (rh < 0) return -1;

    if (lh != rh) {
        err = "black height mismatch";
        return -1;
    }

    return lh + (n->col == color::BLACK ? 1 : 0);
}

bool validate(const node* root, size_t expected_count) {
    const char* err = nullptr;
    return validate(root, expected_count, err);
}

bool validate(const node* root, size_t expected_count, const char*& err_out) {
    err_out = nullptr;

    if (!root) {
        if (expected_count != 0) {
            err_out = "empty tree but expected_count != 0";
            return false;
        }
        return true;
    }

    // Root must be black
    if (root->col != color::BLACK) {
        err_out = "root is not black";
        return false;
    }

    // Root's parent must be nullptr
    if (root->parent != nullptr) {
        err_out = "root parent is not nullptr";
        return false;
    }

    // Check subtree invariants and count nodes
    size_t count = 0;
    int32_t bh = validate_subtree(root, nullptr, count, err_out);
    if (bh < 0) return false;

    if (count != expected_count) {
        err_out = "node count mismatch";
        return false;
    }

    return true;
}

} // namespace rbt
