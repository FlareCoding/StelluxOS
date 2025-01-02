#ifndef VECTOR_H
#define VECTOR_H
#include "kstl_primitive.h"
#include <memory/memory.h>

namespace kstl {
/**
 * @class vector
 * @brief A dynamic array implementation with automatic resizing.
 * 
 * Provides a container for storing elements of type `T` with random access, dynamic resizing,
 * and standard operations such as copy, move, and element access.
 * 
 * @tparam T The type of elements stored in the vector.
 */
template <typename T>
class vector {
public:
    /**
     * @brief Represents a special value indicating no match found in search operations.
     */
    static const size_t npos = static_cast<size_t>(-1);

    /**
     * @brief Constructs an empty vector.
     * 
     * Initializes the vector with no elements and zero capacity.
     */
    vector();

    /**
     * @brief Constructs a vector with an initial capacity.
     * @param initial_capacity The number of elements the vector can initially hold.
     * 
     * Pre-allocates memory to accommodate the specified capacity.
     */
    explicit vector(size_t initial_capacity);

    /**
     * @brief Copy constructor for the vector class.
     * @param other The vector to copy.
     * 
     * Creates a new vector as a copy of the provided vector.
     */
    vector(const vector& other);

    /**
     * @brief Move constructor for the vector class.
     * @param other The vector to move.
     * 
     * Transfers ownership of resources from the provided vector, leaving it in an empty state.
     */
    vector(vector&& other) noexcept;

    /**
     * @brief Destructor for the vector class.
     * 
     * Releases all resources held by the vector.
     */
    ~vector();

    /**
     * @brief Copy assignment operator for the vector class.
     * @param other The vector to copy.
     * @return Reference to the updated vector.
     * 
     * Replaces the current vector with a copy of the provided vector.
     */
    vector& operator=(const vector& other);

    /**
     * @brief Move assignment operator for the vector class.
     * @param other The vector to move.
     * @return Reference to the updated vector.
     * 
     * Transfers ownership of resources from the provided vector, leaving it in an empty state.
     */
    vector& operator=(vector&& other) noexcept;

    /**
     * @brief Accesses an element by index for modification.
     * @param index The index of the element to access.
     * @return Reference to the element at the specified index.
     * 
     * Provides direct access to the element at the given index. The behavior is undefined if the index is out of bounds.
     */
    T& operator[](size_t index);

    /**
     * @brief Accesses an element by index for read-only access.
     * @param index The index of the element to access.
     * @return Reference to the element at the specified index.
     * 
     * Provides read-only access to the element at the given index. The behavior is undefined if the index is out of bounds.
     */
    const T& operator[](size_t index) const;

    /**
     * @brief Retrieves the first element in the vector.
     * @return Reference to the first element.
     * 
     * The behavior is undefined if the vector is empty.
     */
    T& front();

    /**
     * @brief Retrieves the last element in the vector.
     * @return Reference to the last element.
     * 
     * The behavior is undefined if the vector is empty.
     */
    T& back();

    /**
     * @brief Appends a copy of an element to the end of the vector.
     * @param value The element to copy and append.
     * 
     * Adds a new element to the end of the vector, resizing it if necessary.
     */
    void push_back(const T& value);

    /**
     * @brief Appends a movable element to the end of the vector.
     * @param value The element to move and append.
     * 
     * Moves the provided element to the end of the vector, resizing it if necessary.
     */
    void push_back(T&& value);

    /**
     * @brief Inserts an element at a specified position.
     * @param index The position at which to insert the new element.
     * @param value The element to copy and insert.
     * 
     * Shifts existing elements to the right to make space for the new element. The behavior is undefined
     * if the index is out of bounds.
     */
    void insert(size_t index, const T& value);

    /**
     * @brief Removes the last element from the vector.
     * 
     * Reduces the size of the vector by one. The behavior is undefined if the vector is empty.
     */
    void pop_back();

    /**
     * @brief Removes an element at a specified position.
     * @param index The position of the element to remove.
     * 
     * Shifts subsequent elements to the left to fill the gap. The behavior is undefined if the index
     * is out of bounds.
     */
    void erase(size_t index);

    /**
     * @brief Finds the first occurrence of an element in the vector.
     * @param value The element to search for.
     * @return The index of the first occurrence of the element, or `npos` if not found.
     * 
     * Compares elements using the `==` operator.
     */
    size_t find(const T& value) const;

    /**
     * @brief Retrieves a pointer to the internal data array.
     * @return Pointer to the first element of the vector.
     * 
     * Provides direct access to the underlying array storing the vector's elements.
     */
    T* data() const;

    /**
     * @brief Retrieves the number of elements in the vector.
     * @return The current number of elements in the vector.
     * 
     * The size reflects the number of elements currently stored, not the allocated capacity.
     */
    size_t size() const;

    /**
     * @brief Retrieves the current capacity of the vector.
     * @return The number of elements the vector can hold without resizing.
     */
    size_t capacity() const;

    /**
     * @brief Checks if the vector is empty.
     * @return True if the vector contains no elements, false otherwise.
     */
    bool empty() const;

    /**
     * @brief Reserves memory to accommodate at least the specified capacity.
     * @param new_capacity The desired capacity for the vector.
     * 
     * Ensures the vector can store at least `new_capacity` elements without further allocation.
     * Does nothing if the current capacity is already sufficient.
     */
    void reserve(size_t new_capacity);

    /**
     * @brief Resizes the vector to contain the specified number of elements.
     * @param new_size The desired number of elements in the vector.
     * 
     * If the new size is larger, default-constructed elements are added. If smaller, elements are truncated.
     */
    void resize(size_t new_size);

    /**
     * @brief Clears the contents of the vector.
     * 
     * Resets the vector to an empty state, releasing resources used by its elements but retaining its capacity.
     */
    void clear();

    /**
     * @brief Retrieves an iterator to the beginning of the vector.
     * @return Pointer to the first element of the vector.
     * 
     * Allows iteration from the first element.
     */
    T* begin() { return m_data; }

    /**
     * @brief Retrieves an iterator to the end of the vector.
     * @return Pointer to one past the last element of the vector.
     * 
     * Allows iteration until one past the last element.
     */
    T* end() { return m_data + m_size; }

    /**
     * @brief Retrieves a constant iterator to the beginning of the vector.
     * @return Constant pointer to the first element of the vector.
     * 
     * Allows read-only iteration from the first element.
     */
    const T* begin() const { return m_data; }

    /**
     * @brief Retrieves a constant iterator to the end of the vector.
     * @return Constant pointer to one past the last element of the vector.
     * 
     * Allows read-only iteration until one past the last element.
     */
    const T* end() const { return m_data + m_size; }

    /**
     * @brief Retrieves a constant iterator to the beginning of the vector.
     * @return Constant pointer to the first element of the vector.
     * 
     * Allows read-only iteration from the first element.
     */
    const T* cbegin() const { return m_data; }

    /**
     * @brief Retrieves a constant iterator to the end of the vector.
     * @return Constant pointer to one past the last element of the vector.
     * 
     * Allows read-only iteration until one past the last element.
     */
    const T* cend() const { return m_data + m_size; }

private:
    T* m_data;
    size_t m_size;
    size_t m_capacity;

private:
    /**
     * @brief Reallocates the vector's internal storage to a new capacity.
     * @param new_capacity The new capacity for the vector.
     * 
     * Allocates a new memory block with the specified capacity, moves existing elements
     * to the new storage, and deallocates the old memory. This method is called internally
     * when the vector's capacity needs to be increased.
     */
    void _reallocate(size_t new_capacity);
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
T& vector<T>::front() {
    // Assuming the vector is not empty
    return m_data[0];
}

template <typename T>
T& vector<T>::back() {
    // Assuming the vector is not empty
    return m_data[m_size - 1];
}

template <typename T>
void vector<T>::push_back(const T& value) {
    if (m_size == m_capacity) {
        size_t new_capacity = m_capacity == 0 ? 1 : 2 * m_capacity;
        _reallocate(new_capacity);
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
        _reallocate(new_capacity);
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
        _reallocate(new_capacity);
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
        _reallocate(new_capacity);
    }
}

template <typename T>
void vector<T>::resize(size_t new_size) {
    if (new_size < m_size) {
        // Shrink the vector: Destroy the excess elements
        for (size_t i = new_size; i < m_size; ++i) {
            m_data[i].~T();
        }
    } else if (new_size > m_size) {
        // Grow the vector: Ensure capacity and default-construct new elements
        if (new_size > m_capacity) {
            size_t new_capacity = m_capacity == 0 ? 1 : m_capacity;
            while (new_capacity < new_size) {
                new_capacity *= 2;
            }
            _reallocate(new_capacity);
        }

        // Default-construct new elements
        for (size_t i = m_size; i < new_size; ++i) {
            if constexpr (!is_primitive<T>::value) {
                new(&m_data[i]) T();
            }
        }
    }

    m_size = new_size;
}

template <typename T>
void vector<T>::clear() {
    for (size_t i = 0; i < m_size; ++i) {
        m_data[i].~T();
    }
    m_size = 0;
}

template <typename T>
void vector<T>::_reallocate(size_t new_capacity) {
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
