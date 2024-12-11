#ifndef VECTOR_H
#define VECTOR_H
#include "kstl_primitive.h"
#include <memory/memory.h>

namespace kstl {
template <typename T>
class vector {
public:
    static const size_t npos = static_cast<size_t>(-1);

    vector();
    explicit vector(size_t initial_capacity);
    vector(const vector& other);
    vector(vector&& other) noexcept;

    ~vector();

    vector& operator=(const vector& other);
    vector& operator=(vector&& other) noexcept;
    T& operator[](size_t index);
    const T& operator[](size_t index) const;

    void push_back(const T& value);
    void push_back(T&& value);

    void insert(size_t index, const T& value);

    void pop_back();
    void erase(size_t index);

    size_t find(const T& value) const;

    T* data() const;
    size_t size() const;
    size_t capacity() const;
    bool empty() const;

    void reserve(size_t new_capacity);
    void clear();

    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }

    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }

    const T* cbegin() const { return m_data; }
    const T* cend() const { return m_data + m_size; }

private:
    T* m_data;
    size_t m_size;
    size_t m_capacity;

private:
    void reallocate(size_t new_capacity);
};

template <typename T>
vector<T>::vector()
    : m_data(nullptr), m_size(0), m_capacity(0) {}

template <typename T>
vector<T>::vector(size_t initial_capacity)
    : m_data(nullptr), m_size(0), m_capacity(initial_capacity) {
    if (initial_capacity > 0) {
        m_data = static_cast<T*>(zmalloc(initial_capacity * sizeof(T)));
    }
}

template <typename T>
vector<T>::vector(const vector<T>& other)
    : m_data(nullptr), m_size(other.m_size), m_capacity(other.m_capacity) {
    if (m_capacity > 0) {
        m_data = static_cast<T*>(zmalloc(m_capacity * sizeof(T)));

        for (size_t i = 0; i < m_size; ++i) {
            if constexpr (is_primitive<T>::value) {
                m_data[i] = other.m_data[i];
            } else {
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
        for (size_t i = 0; i < m_size; i++) {
            m_data[i].~T();
        }
        free(m_data);
    }
}

template <typename T>
vector<T>& vector<T>::operator=(const vector<T>& other) {
    if (this != &other) {
        if (m_data) {
            for (size_t i = 0; i < m_size; i++) {
                m_data[i].~T();
            }
            free(m_data);
        }

        m_size = other.m_size;
        m_capacity = other.m_capacity;
        m_data = m_capacity > 0 ? static_cast<T*>(zmalloc(m_capacity * sizeof(T))) : nullptr;

        for (size_t i = 0; i < m_size; i++) {
            if constexpr (is_primitive<T>::value) {
                m_data[i] = other.m_data[i];
            } else {
                new(&m_data[i]) T(other.m_data[i]);
            }
        }
    }

    return *this;
}

template <typename T>
vector<T>& vector<T>::operator=(vector<T>&& other) noexcept {
    if (this != &other) {
        if (m_data) {
            for (size_t i = 0; i < m_size; i++) {
                m_data[i].~T();
            }
            free(m_data);
        }

        m_data = other.m_data;
        m_size = other.m_size;
        m_capacity = other.m_capacity;

        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }

    return *this;
}

template <typename T>
T& vector<T>::operator[](size_t index) {
    return m_data[index];
}

template <typename T>
const T& vector<T>::operator[](size_t index) const {
    return m_data[index];
}

template <typename T>
void vector<T>::push_back(const T& value) {
    if (m_size == m_capacity) {
        size_t new_capacity = m_capacity == 0 ? 1 : 2 * m_capacity;
        reallocate(new_capacity);
    }

    if constexpr (is_primitive<T>::value) {
        m_data[m_size] = value;
    } else {
        new(&m_data[m_size]) T(value);
    }

    ++m_size;
}

template <typename T>
void vector<T>::push_back(T&& value) {
    if (m_size == m_capacity) {
        size_t new_capacity = m_capacity == 0 ? 1 : 2 * m_capacity;
        reallocate(new_capacity);
    }

    if constexpr (is_primitive<T>::value) {
        m_data[m_size] = static_cast<T&&>(value);
    } else {
        new(&m_data[m_size]) T(static_cast<T&&>(value));
    }

    ++m_size;
}

template <typename T>
void vector<T>::insert(size_t index, const T& value) {
    if (index > m_size) {
        // Index out of range, do nothing or handle error appropriately
        return;
    }

    if (m_size == m_capacity) {
        size_t new_capacity = m_capacity == 0 ? 1 : 2 * m_capacity;
        reallocate(new_capacity);
    }

    // Shift elements to the right
    for (size_t i = m_size; i > index; --i) {
        if constexpr (is_primitive<T>::value) {
            m_data[i] = m_data[i - 1];
        } else {
            new(&m_data[i]) T(static_cast<T&&>(m_data[i - 1]));
            m_data[i - 1].~T();
        }
    }

    // Insert the new element
    if constexpr (is_primitive<T>::value) {
        m_data[index] = value;
    } else {
        new(&m_data[index]) T(value);
    }

    ++m_size;
}

template <typename T>
void vector<T>::pop_back() {
    if (m_size > 0) {
        m_data[m_size - 1].~T();
        --m_size;
    }
}

template <typename T>
void vector<T>::erase(size_t index) {
    if (index < m_size) {
        // Destroy the element at index
        m_data[index].~T();

        // Shift elements left
        for (size_t i = index; i < m_size - 1; ++i) {
            if constexpr (is_primitive<T>::value) {
                m_data[i] = m_data[i + 1];
            } else {
                // Move element at i+1 into position i
                new(&m_data[i]) T(static_cast<T&&>(m_data[i + 1]));
                m_data[i + 1].~T();
            }
        }

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
void vector<T>::reserve(size_t new_capacity) {
    if (new_capacity > m_capacity) {
        reallocate(new_capacity);
    }
}

template <typename T>
void vector<T>::clear() {
    for (size_t i = 0; i < m_size; ++i) {
        m_data[i].~T();
    }
    m_size = 0;
}

template <typename T>
void vector<T>::reallocate(size_t new_capacity) {
    T* new_block = static_cast<T*>(zmalloc(new_capacity * sizeof(T)));

    if (new_block) {
        for (size_t i = 0; i < m_size; ++i) {
            if constexpr (is_primitive<T>::value) {
                new_block[i] = m_data[i];
            } else {
                new(&new_block[i]) T(static_cast<T&&>(m_data[i]));
                m_data[i].~T();
            }
        }

        if (m_data) {
            free(m_data);
        }

        m_data = new_block;
        m_capacity = new_capacity;
    }
}
} // namespace kstl

#endif // VECTOR_H
