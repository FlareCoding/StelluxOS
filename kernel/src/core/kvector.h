#ifndef KVECTOR_H
#define KVECTOR_H
#include "kstl_primitive.h"
#include <memory/kmemory.h>

namespace kstl {
template <typename T>
class vector {
public:
    static const size_t npos = static_cast<size_t>(-1);

    vector();
    explicit vector(size_t initialCapacity);
    vector(const vector& other);
    vector(vector&& other) noexcept;

    ~vector();

    vector& operator=(const vector& other);
    vector& operator=(vector&& other) noexcept;
    T& operator[](size_t index);
    const T& operator[](size_t index) const;

    void pushBack(const T& value);
    void pushBack(T&& value);

    void insert(size_t index, const T& value);

    void popBack();
    void erase(size_t index);

    size_t find(const T& value) const;

    T* data() const;
    size_t size() const;
    size_t capacity() const;
    bool empty() const;

    void reserve(size_t newCapacity);
    void clear();

private:
    T* m_data;
    size_t m_size;
    size_t m_capacity;

private:
    void _reallocate(size_t newCapacity);
};

template <typename T>
vector<T>::vector()
    : m_data(nullptr), m_size(0), m_capacity(0) {}

template <typename T>
vector<T>::vector(size_t initialCapacity)
    : m_data(nullptr), m_size(0), m_capacity(initialCapacity) {
    if (initialCapacity > 0) {
        m_data = static_cast<T*>(kmalloc(initialCapacity * sizeof(T)));
    }
}

template <typename T>
vector<T>::vector(const vector<T>& other)
    : m_data(nullptr), m_size(other.m_size), m_capacity(other.m_capacity) {
    if (m_capacity > 0) {
        m_data = static_cast<T*>(kmalloc(m_capacity * sizeof(T)));

        for (size_t i = 0; i < m_size; ++i) {
            if constexpr (is_primitive<T>::value) {
                // Direct assignment for primitive types
                m_data[i] = other.m_data[i];
            } else {
                // Placement new for copy construction for complex types
                new(&m_data[i]) T(other.m_data[i]);
            }
        }
    }
}

template <typename T>
vector<T>::vector(vector<T>&& other) noexcept
    : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity) {
    other.m_data = nullptr;
    other.m_size = 0;
    other.m_capacity = 0;
}

template <typename T>
vector<T>::~vector() {
    if (m_data) {
        // Destruct each element in the vector
        for (size_t i = 0; i < m_size; i++) {
            m_data[i].~T();
        }

        // Free the allocated memory
        kfree(m_data);
    }
}

template <typename T>
vector<T>& vector<T>::operator=(const vector<T>& other) {
    // Check for self-assignment
    if (this != &other) {
        if (m_data) {
            // Destruct existing elements
            for (size_t i = 0; i < m_size; i++) {
                m_data[i].~T();
            }

            // Free existing memory
            kfree(m_data);
        }

        // Allocate new memory and copy elements
        m_size = other.m_size;
        m_capacity = other.m_capacity;
        m_data = m_capacity > 0 ? static_cast<T*>(kmalloc(m_capacity * sizeof(T))) : nullptr;

        for (size_t i = 0; i < m_size; i++) {
            if constexpr (is_primitive<T>::value) {
                // Direct assignment for primitive types
                m_data[i] = other.m_data[i];
            } else {
                // Placement new for copy construction for complex types
                new(&m_data[i]) T(other.m_data[i]);
            }
        }
    }

    return *this;
}

template <typename T>
vector<T>& vector<T>::operator=(vector<T>&& other) noexcept {
    // Check for self-assignment
    if (this != &other) {
        if (m_data) {
            // Destruct existing elements and free existing memory
            for (size_t i = 0; i < m_size; i++) {
                m_data[i].~T();
            }

            kfree(m_data);
        }

        // Transfer ownership of resources
        m_data = other.m_data;
        m_size = other.m_size;
        m_capacity = other.m_capacity;

        // Reset the moved-from vector
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }

    return *this;
}

template <typename T>
T& vector<T>::operator[](size_t index) {
    // Unchecked access for performance
    return m_data[index];
}

template <typename T>
const T& vector<T>::operator[](size_t index) const {
    // Unchecked access for performance
    return m_data[index];
}

template <typename T>
void vector<T>::pushBack(const T& value) {
    if (m_size == m_capacity) {
        // Grow the capacity. Common strategies include doubling the size.
        size_t newCapacity = m_capacity == 0 ? 1 : 2 * m_capacity;

        _reallocate(newCapacity);
    }

    if constexpr (is_primitive<T>::value) {
        // Direct assignment for primitive types
        m_data[m_size] = value;
    } else {
        // Placement new for copy construction for complex types
        new(&m_data[m_size]) T(value);
    }

    ++m_size;
}

template <typename T>
void vector<T>::pushBack(T&& value) {
    if (m_size == m_capacity) {
        size_t newCapacity = m_capacity == 0 ? 1 : 2 * m_capacity;
        _reallocate(newCapacity);
    }

    if constexpr (is_primitive<T>::value) {
        // Direct assignment for primitive types
        m_data[m_size] = static_cast<T&&>(value);
    } else {
        // Placement new for complex types
        new(&m_data[m_size]) T(static_cast<T&&>(value));
    }

    ++m_size;
}

template <typename T>
void vector<T>::insert(size_t index, const T& value) {
    if (index > m_size) {
        return;
    }

    if (m_size == m_capacity) {
        // Increase the capacity if necessary
        size_t newCapacity = m_capacity == 0 ? 1 : 2 * m_capacity;
        _reallocate(newCapacity);
    }

    // Shift elements to the right to make space for the new element
    for (size_t i = m_size; i > index; --i) {
        if constexpr (is_primitive<T>::value) {
            m_data[i] = m_data[i - 1];
        } else {
            // Move to the right
            new(&m_data[i]) T(static_cast<T&&>(m_data[i - 1]));

            // Destruct the moved-from element
            m_data[i - 1].~T();
        }
    }

    // Insert the new element
    if constexpr (is_primitive<T>::value) {
        m_data[index] = value;
    } else {
        new(&m_data[index]) T(value); // Placement new for copy construction
    }

    ++m_size;
}

template <typename T>
void vector<T>::popBack() {
    if (m_size > 0) {
        // Call the destructor of the last element
        m_data[m_size - 1].~T();
        
        // Reduce the size
        --m_size;
    }
}

template <typename T>
void vector<T>::erase(size_t index) {
    if (index < m_size) {
        // Call the destructor for the element to be removed
        m_data[index].~T();

        // Shift elements to the left to fill the gap
        for (size_t i = index; i < m_size - 1; ++i) {
            if constexpr (is_primitive<T>::value) {
                // Direct assignment for primitive types
                m_data[i] = m_data[i + 1];
            } else {
                // Placement new with copy assignment for complex types
                new(&m_data[i]) T(m_data[i + 1]);

                // Destruct the moved-from element
                m_data[i + 1].~T();
            }
            
            // Destruct the moved-from element
            m_data[i + 1].~T();
        }

        // Reduce the size of the vector
        --m_size;
    }
}

template <typename T>
size_t vector<T>::find(const T& value) const {
    for (size_t i = 0; i < m_size; ++i) {
        if (m_data[i] == value) {
            return i;
        }
    }

    return vector<T>::npos;
}

template <typename T>
T* vector<T>::data() const {
    return m_data;
}

template <typename T>
size_t vector<T>::size() const {
    return m_size;
}

template <typename T>
size_t vector<T>::capacity() const {
    return m_capacity;
}

template <typename T>
bool vector<T>::empty() const {
    return m_size == 0;
}

template <typename T>
void vector<T>::reserve(size_t newCapacity) {
    if (newCapacity > m_capacity) {
        _reallocate(newCapacity);  // A function to handle reallocation
    }
}

template <typename T>
void vector<T>::clear() {
    // Destruct each element
    for (size_t i = 0; i < m_size; ++i) {
        m_data[i].~T();
    }

    // Reset the size, but keep the allocated memory
    m_size = 0;
}

template <typename T>
void vector<T>::_reallocate(size_t newCapacity) {
    T* newBlock = static_cast<T*>(kmalloc(newCapacity * sizeof(T)));

    if (newBlock) {
        // Move existing elements to the new block
        for (size_t i = 0; i < m_size; ++i) {
            if constexpr (is_primitive<T>::value) {
                // Direct assignment for primitive types
                newBlock[i] = m_data[i];
            } else {
                // Move construct each element for complex types
                new(&newBlock[i]) T(static_cast<T&&>(m_data[i]));

                // Destruct the old element
                m_data[i].~T();
            }
        }

        // Free the old block
        if (m_data) {
            kfree(m_data);
        }

        // Update the vector to point to the new block
        m_data = newBlock;
        m_capacity = newCapacity;
    }
}
} // namespace kstl

#endif
