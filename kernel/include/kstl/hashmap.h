#ifndef HASHMAP_H
#define HASHMAP_H
#include "kstl_primitive.h"
#include "vector.h"
#include <memory/memory.h>
#include <string.h>

namespace kstl {

/**
 * @brief A dynamic-size hashmap implementation using separate chaining for collision resolution.
 * 
 * This templated hashmap supports insertion, retrieval, and removal of key-value pairs.
 * The table dynamically resizes when the load factor exceeds a threshold.
 * 
 * @tparam K Type of keys in the hashmap.
 * @tparam V Type of values in the hashmap.
 */
template <typename K, typename V>
class hashmap {
public:
    /**
     * @brief Constructs a hashmap with an initial capacity and load factor.
     * 
     * @param initial_capacity Initial number of buckets in the hashmap.
     * @param load_factor Maximum load factor before resizing occurs.
     */
    explicit hashmap(size_t initial_capacity = 16, double load_factor = 0.75);

    /**
     * @brief Copy constructor.
     * 
     * @param other The hashmap to copy.
     */
    hashmap(const hashmap& other);

    /**
     * @brief Move constructor.
     * 
     * @param other The hashmap to move.
     */
    hashmap(hashmap&& other) noexcept;

    /**
     * @brief Copy assignment operator.
     * 
     * @param other The hashmap to copy.
     * @return Reference to this hashmap.
     */
    hashmap& operator=(const hashmap& other);

    /**
     * @brief Move assignment operator.
     * 
     * @param other The hashmap to move.
     * @return Reference to this hashmap.
     */
    hashmap& operator=(hashmap&& other) noexcept;

    /**
     * @brief Destroys the hashmap and frees all allocated memory.
     */
    ~hashmap();

    /**
     * @brief Inserts a key-value pair into the hashmap.
     * 
     * @param key The key to insert.
     * @param value The value associated with the key.
     * @return True if the key-value pair was inserted, false if the key already exists.
     */
    bool insert(const K& key, const V& value);

    /**
     * @brief Retrieves the value associated with a given key.
     * 
     * @param key The key to search for.
     * @return A pointer to the value if the key exists, or nullptr if not found.
     */
    V* get(const K& key) const;

    /**
     * @brief Removes a key-value pair from the hashmap.
     * 
     * @param key The key to remove.
     * @return True if the key was found and removed, false otherwise.
     */
    bool remove(const K& key);

    /**
     * @brief Finds if a key exists in the hashmap.
     * 
     * @param key The key to find.
     * @return True if the key exists, false otherwise.
     */
    bool find(const K& key) const;

    /**
     * @brief Returns the number of elements in the hashmap.
     * 
     * @return The size of the hashmap.
     */
    size_t size() const;

    /**
     * @brief Provides access to elements using the subscript operator.
     * 
     * If the key does not exist, a new element is created with the default value.
     * 
     * @param key The key to access.
     * @return Reference to the value associated with the key.
     */
    V& operator[](const K& key);

    /**
    * @brief Retrieves all keys in the hashmap as a kstl::vector.
    * 
    * @return A kstl::vector containing all keys.
    */
    kstl::vector<K> keys() const;

private:
    /**
     * @brief Represents a node in a bucket's linked list.
     */
    struct node {
        K key;      ///< Key of the node.
        V value;    ///< Value associated with the key.
        node* next; ///< Pointer to the next node in the chain.
    };

    size_t m_bucket_count; ///< Number of buckets in the hashmap.
    size_t m_size;         ///< Current number of elements in the hashmap.
    double m_load_factor;  ///< Load factor threshold for resizing.
    node** m_buckets;      ///< Array of bucket pointers.

    /**
     * @brief Computes the hash index for a given key.
     * 
     * @param key The key to hash.
     * @return The bucket index for the key.
     */
    size_t _hash(const K& key) const;

    /**
     * @brief Default hash function for uint64_t keys.
     * 
     * @param key The key to hash.
     * @return The computed hash value.
     */
    size_t _default_hash(uint64_t key) const;

    /**
     * @brief Default hash function for string keys.
     * 
     * @param key The key to hash.
     * @return The computed hash value.
     */
    size_t _default_hash(const kstl::string& key) const;

    /**
     * @brief Fallback hash function for other key types.
     * 
     * @tparam T Type of the key.
     * @param key The key to hash.
     * @return The computed hash value.
     */
    template <typename T>
    size_t _default_hash(const T& key) const;

    /**
     * @brief Resizes the hashmap and rehashes all elements.
     */
    void _rehash();

    /**
     * @brief Frees all nodes and buckets.
     */
    void _clear();

    /**
     * @brief Deep copies the contents from another hashmap.
     * 
     * @param other The hashmap to copy.
     */
    void _copy_from(const hashmap& other);
};

// Implementation of hashmap methods

/**
 * @copydoc hashmap::hashmap(size_t, double)
 */
template <typename K, typename V>
hashmap<K, V>::hashmap(size_t initial_capacity, double load_factor)
    : m_bucket_count(initial_capacity), m_size(0), m_load_factor(load_factor) {
    m_buckets = new node*[m_bucket_count];
    for (size_t i = 0; i < m_bucket_count; ++i) {
        m_buckets[i] = nullptr;
    }
}

/**
 * @copydoc hashmap::hashmap(const hashmap&)
 */
template <typename K, typename V>
hashmap<K, V>::hashmap(const hashmap& other) {
    _copy_from(other);
}

/**
 * @copydoc hashmap::hashmap(hashmap&&)
 */
template <typename K, typename V>
hashmap<K, V>::hashmap(hashmap&& other) noexcept
    : m_bucket_count(other.m_bucket_count), m_size(other.m_size), m_load_factor(other.m_load_factor),
      m_buckets(other.m_buckets) {
    other.m_buckets = nullptr;
    other.m_size = 0;
    other.m_bucket_count = 0;
}

/**
 * @copydoc hashmap::operator=(const hashmap&)
 */
template <typename K, typename V>
hashmap<K, V>& hashmap<K, V>::operator=(const hashmap& other) {
    if (this != &other) {
        _clear();
        _copy_from(other);
    }
    return *this;
}

/**
 * @copydoc hashmap::operator=(hashmap&&)
 */
template <typename K, typename V>
hashmap<K, V>& hashmap<K, V>::operator=(hashmap&& other) noexcept {
    if (this != &other) {
        _clear();
        m_bucket_count = other.m_bucket_count;
        m_size = other.m_size;
        m_load_factor = other.m_load_factor;
        m_buckets = other.m_buckets;

        other.m_buckets = nullptr;
        other.m_size = 0;
        other.m_bucket_count = 0;
    }
    return *this;
}

/**
 * @copydoc hashmap::~hashmap()
 */
template <typename K, typename V>
hashmap<K, V>::~hashmap() {
    _clear();
}

/**
 * @copydoc hashmap::insert(const K&, const V&)
 */
template <typename K, typename V>
bool hashmap<K, V>::insert(const K& key, const V& value) {
    size_t index = _hash(key);
    node* curr_node = m_buckets[index];

    while (curr_node) {
        if (curr_node->key == key) {
            return false;
        }
        curr_node = curr_node->next;
    }

    node* new_node = new node{key, value, m_buckets[index]};
    m_buckets[index] = new_node;
    ++m_size;

    if (static_cast<double>(m_size) / m_bucket_count > m_load_factor) {
        _rehash();
    }

    return true;
}

/**
 * @copydoc hashmap::get(const K&) const
 */
template <typename K, typename V>
V* hashmap<K, V>::get(const K& key) const {
    size_t index = _hash(key);
    node* curr_node = m_buckets[index];

    while (curr_node) {
        if (curr_node->key == key) {
            return &curr_node->value;
        }
        curr_node = curr_node->next;
    }

    return nullptr;
}

/**
 * @copydoc hashmap::remove(const K&)
 */
template <typename K, typename V>
bool hashmap<K, V>::remove(const K& key) {
    size_t index = _hash(key);
    node* curr_node = m_buckets[index];
    node* prev = nullptr;

    while (curr_node) {
        if (curr_node->key == key) {
            if (prev) {
                prev->next = curr_node->next;
            } else {
                m_buckets[index] = curr_node->next;
            }

            delete curr_node;
            --m_size;
            return true;
        }
        prev = curr_node;
        curr_node = curr_node->next;
    }

    return false;
}

/**
 * @copydoc hashmap::find(const K&) const
 */
template <typename K, typename V>
bool hashmap<K, V>::find(const K& key) const {
    return get(key) != nullptr;
}

/**
 * @copydoc hashmap::size() const
 */
template <typename K, typename V>
size_t hashmap<K, V>::size() const {
    return m_size;
}

/**
 * @copydoc hashmap::operator[](const K&)
 */
template <typename K, typename V>
V& hashmap<K, V>::operator[](const K& key) {
    size_t index = _hash(key);
    node* curr_node = m_buckets[index];

    while (curr_node) {
        if (curr_node->key == key) {
            return curr_node->value;
        }
        curr_node = curr_node->next;
    }

    // Key does not exist, create a new node
    node* new_node = new node{key, V{}, m_buckets[index]};
    m_buckets[index] = new_node;
    ++m_size;

    if (static_cast<double>(m_size) / m_bucket_count > m_load_factor) {
        _rehash();
    }

    return new_node->value;
}

template <typename K, typename V>
kstl::vector<K> hashmap<K, V>::keys() const {
    kstl::vector<K> key_list;
    // If your kstl::vector supports reserve, do so for efficiency
    key_list.reserve(m_size);

    for (size_t i = 0; i < m_bucket_count; ++i) {
        node* curr_node = m_buckets[i];
        while (curr_node) {
            key_list.push_back(curr_node->key);
            curr_node = curr_node->next;
        }
    }
    return key_list;
}

/**
 * @copydoc hashmap::_hash(const K&) const
 */
template <typename K, typename V>
size_t hashmap<K, V>::_hash(const K& key) const {
    return _default_hash(key) % m_bucket_count;
}

/**
 * @copydoc hashmap::_default_hash(uint64_t) const
 */
template <typename K, typename V>
size_t hashmap<K, V>::_default_hash(uint64_t key) const {
    key ^= (key >> 33);
    key *= 0xff51afd7ed558ccd;
    key ^= (key >> 33);
    key *= 0xc4ceb9fe1a85ec53;
    key ^= (key >> 33);
    return key;
}

/**
 * @copydoc hashmap::_default_hash(const kstl::string&) const
 */
template <typename K, typename V>
size_t hashmap<K, V>::_default_hash(const kstl::string& key) const {
    const char* str = key.c_str();  // Assuming kstl::string has this method
    size_t hash = 0;
    while (*str) {
        hash = (hash * 31) + static_cast<size_t>(*str);
        ++str;
    }
    return hash;
}

/**
 * @copydoc hashmap::_default_hash(const T&) const
 */
template <typename K, typename V>
template <typename T>
size_t hashmap<K, V>::_default_hash(const T& key) const {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&key);
    size_t hash = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        hash = (hash * 31) + data[i];
    }
    return hash;
}

/**
 * @copydoc hashmap::_rehash()
 */
template <typename K, typename V>
void hashmap<K, V>::_rehash() {
    size_t new_bucket_count = m_bucket_count * 2;
    node** new_buckets = new node*[new_bucket_count];
    for (size_t i = 0; i < new_bucket_count; ++i) {
        new_buckets[i] = nullptr;
    }

    for (size_t i = 0; i < m_bucket_count; ++i) {
        node* curr_node = m_buckets[i];
        while (curr_node) {
            node* next = curr_node->next;

            size_t new_index = _default_hash(curr_node->key) % new_bucket_count;
            curr_node->next = new_buckets[new_index];
            new_buckets[new_index] = curr_node;

            curr_node = next;
        }
    }

    delete[] m_buckets;
    m_buckets = new_buckets;
    m_bucket_count = new_bucket_count;
}

/**
 * @copydoc hashmap::_clear()
 */
template <typename K, typename V>
void hashmap<K, V>::_clear() {
    for (size_t i = 0; i < m_bucket_count; ++i) {
        node* curr_node = m_buckets[i];
        while (curr_node) {
            node* next = curr_node->next;
            delete curr_node;
            curr_node = next;
        }
    }
    delete[] m_buckets;
    m_buckets = nullptr;
    m_size = 0;
    m_bucket_count = 0;
}

/**
 * @copydoc hashmap::_copy_from(const hashmap&)
 */
template <typename K, typename V>
void hashmap<K, V>::_copy_from(const hashmap& other) {
    m_bucket_count = other.m_bucket_count;
    m_size = other.m_size;
    m_load_factor = other.m_load_factor;
    m_buckets = new node*[m_bucket_count];

    for (size_t i = 0; i < m_bucket_count; ++i) {
        m_buckets[i] = nullptr;
        node* curr_node = other.m_buckets[i];
        while (curr_node) {
            insert(curr_node->key, curr_node->value);
            curr_node = curr_node->next;
        }
    }
}

} // namespace kstl

#endif // HASHMAP_H
