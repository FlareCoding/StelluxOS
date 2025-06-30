#include <unit_tests/unit_tests.h>
#include <memory/memory.h>
#include <kstl/ring_buffer.h>
#include <process/process.h>
#include <sched/sched.h>

using namespace kstl;

// Helper class to test non-primitive types and memory management
class RingTestObject {
public:
    static size_t alive_count;
    static size_t total_constructions;
    static size_t total_destructions;
    int value;
    int id;

    RingTestObject(int val = 0) : value(val), id(total_constructions) {
        ++alive_count;
        ++total_constructions;
    }

    RingTestObject(const RingTestObject& other) : value(other.value), id(total_constructions) {
        ++alive_count;
        ++total_constructions;
    }

    RingTestObject(RingTestObject&& other) noexcept : value(other.value), id(total_constructions) {
        ++alive_count;
        ++total_constructions;
        other.value = -1; // Mark as moved
    }

    ~RingTestObject() {
        --alive_count;
        ++total_destructions;
    }

    RingTestObject& operator=(const RingTestObject& other) {
        if (this != &other) {
            value = other.value;
        }
        return *this;
    }

    bool operator==(const RingTestObject& other) const {
        return value == other.value;
    }

    static void reset_counters() {
        alive_count = 0;
        total_constructions = 0;
        total_destructions = 0;
    }
};

size_t RingTestObject::alive_count = 0;
size_t RingTestObject::total_constructions = 0;
size_t RingTestObject::total_destructions = 0;

// Test basic construction and properties
DECLARE_UNIT_TEST("ring_buffer basic construction", test_ring_buffer_basic_construction) {
    ring_buffer<int> ring_default;
    ASSERT_EQ(ring_default.size(), (size_t)0, "Default ring buffer should be empty");
    ASSERT_TRUE(ring_default.empty(), "Default ring buffer should report empty");
    ASSERT_FALSE(ring_default.full(), "Default ring buffer should not be full");
    ASSERT_EQ(ring_default.capacity(), (size_t)1024, "Default capacity should be 1024");

    ring_buffer<int> ring_custom(256);
    ASSERT_EQ(ring_custom.capacity(), (size_t)256, "Custom capacity should be 256");
    
    return UNIT_TEST_SUCCESS;
}

// Test single producer/consumer operations
DECLARE_UNIT_TEST("ring_buffer single producer consumer", test_ring_buffer_single_producer_consumer) {
    ring_buffer<int> ring(8);
    
    ASSERT_TRUE(ring.push_single_producer(42), "Should be able to push to empty buffer");
    ASSERT_EQ(ring.size(), (size_t)1, "Size should be 1 after one push");
    ASSERT_FALSE(ring.empty(), "Should not be empty after push");

    int value;
    ASSERT_TRUE(ring.pop_single_consumer(value), "Should be able to pop from non-empty buffer");
    ASSERT_EQ(value, 42, "Popped value should match pushed value");
    ASSERT_EQ(ring.size(), (size_t)0, "Size should be 0 after pop");
    ASSERT_TRUE(ring.empty(), "Should be empty after pop");

    return UNIT_TEST_SUCCESS;
}

// Test multi-producer/consumer operations
DECLARE_UNIT_TEST("ring_buffer multi producer consumer", test_ring_buffer_multi_producer_consumer) {
    ring_buffer<int> ring(16);

    ASSERT_TRUE(ring.push(100), "Multi-producer push should work");
    ASSERT_TRUE(ring.push(200), "Multi-producer push should work");
    ASSERT_EQ(ring.size(), (size_t)2, "Size should be 2 after two pushes");

    int value1, value2;
    ASSERT_TRUE(ring.pop(value1), "Multi-consumer pop should work");
    ASSERT_TRUE(ring.pop(value2), "Multi-consumer pop should work");
    ASSERT_EQ(value1, 100, "First popped value should be 100");
    ASSERT_EQ(value2, 200, "Second popped value should be 200");
    ASSERT_TRUE(ring.empty(), "Buffer should be empty after pops");

    return UNIT_TEST_SUCCESS;
}

// Test bulk operations
DECLARE_UNIT_TEST("ring_buffer bulk operations", test_ring_buffer_bulk_operations) {
    ring_buffer<int> ring(32);
    
    int write_data[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    size_t written = ring.write_bulk(write_data, 10);
    ASSERT_EQ(written, (size_t)10, "Should write all 10 elements");
    ASSERT_EQ(ring.size(), (size_t)10, "Size should be 10 after bulk write");

    int read_data[10];
    size_t read = ring.read_bulk(read_data, 10);
    ASSERT_EQ(read, (size_t)10, "Should read all 10 elements");
    ASSERT_EQ(ring.size(), (size_t)0, "Size should be 0 after bulk read");

    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(read_data[i], i, "Read data should match written data");
    }

    return UNIT_TEST_SUCCESS;
}

// Test edge cases and error conditions
DECLARE_UNIT_TEST("ring_buffer edge cases", test_ring_buffer_edge_cases) {
    ring_buffer<int> ring(4);

    ASSERT_EQ(ring.write_bulk(nullptr, 5), (size_t)0, "Writing null pointer should return 0");
    ASSERT_EQ(ring.read_bulk(nullptr, 5), (size_t)0, "Reading to null pointer should return 0");

    int dummy_array[5] = {1, 2, 3, 4, 5};
    ASSERT_EQ(ring.write_bulk(dummy_array, 0), (size_t)0, "Writing 0 elements should return 0");
    ASSERT_EQ(ring.read_bulk(dummy_array, 0), (size_t)0, "Reading 0 elements should return 0");

    for (int i = 0; i < 4; i++) {
        ring.push(i);
    }
    ASSERT_TRUE(ring.full(), "Buffer should be full");

    int value;
    ring.pop(value);
    ring.pop(value);
    ASSERT_EQ(ring.size(), (size_t)2, "Should have 2 elements after removing 2");

    ring.push(100);
    ring.push(101);
    ASSERT_TRUE(ring.full(), "Buffer should be full again");

    return UNIT_TEST_SUCCESS;
}

// Test non-primitive types and memory management
DECLARE_UNIT_TEST("ring_buffer non primitive types", test_ring_buffer_non_primitive_types) {
    RingTestObject::reset_counters();
    
    {
        ring_buffer<RingTestObject> ring(8);
        ASSERT_EQ(RingTestObject::alive_count, (size_t)0, "No objects should be alive initially");

        // Test pushing objects
        ring.push(RingTestObject(42));
        ASSERT_EQ(RingTestObject::alive_count, (size_t)1, "One object should be alive after push");

        ring.push(RingTestObject(84));
        ASSERT_EQ(RingTestObject::alive_count, (size_t)2, "Two objects should be alive");

        // Test popping objects
        RingTestObject obj;
        ASSERT_EQ(RingTestObject::alive_count, (size_t)3, "Three objects alive (including local obj)");
        
        ASSERT_TRUE(ring.pop(obj), "Should be able to pop object");
        ASSERT_EQ(obj.value, 42, "Popped object should have correct value");
        ASSERT_EQ(RingTestObject::alive_count, (size_t)2, "Two objects should remain after pop");

        // Test bulk operations with objects
        RingTestObject objects[3] = {RingTestObject(100), RingTestObject(200), RingTestObject(300)};
        size_t written = ring.write_bulk(objects, 3);
        ASSERT_EQ(written, (size_t)3, "Should write all 3 objects");
        ASSERT_EQ(RingTestObject::alive_count, (size_t)8, "Should have original objects + copies in ring + local array");

        // Test clearing
        ring.clear();
        ASSERT_EQ(ring.size(), (size_t)0, "Ring should be empty after clear");
        ASSERT_TRUE(ring.empty(), "Ring should report empty after clear");
        // Note: alive_count may vary due to temporary objects, but destructions should balance constructions
    }

    // All ring objects should be destroyed when ring goes out of scope
    ASSERT_EQ(RingTestObject::alive_count, (size_t)0, "All objects should be destroyed after ring destruction");
    
    return UNIT_TEST_SUCCESS;
}

// Test reset and clear operations
DECLARE_UNIT_TEST("ring_buffer reset and clear", test_ring_buffer_reset_clear) {
    ring_buffer<int> ring(8);

    // Fill the buffer
    for (int i = 0; i < 8; i++) {
        ring.push(i);
    }
    ASSERT_EQ(ring.size(), (size_t)8, "Buffer should be full");
    ASSERT_TRUE(ring.full(), "Buffer should report full");

    // Test clear
    ring.clear();
    ASSERT_EQ(ring.size(), (size_t)0, "Buffer should be empty after clear");
    ASSERT_TRUE(ring.empty(), "Buffer should report empty after clear");
    ASSERT_FALSE(ring.full(), "Buffer should not be full after clear");
    ASSERT_EQ(ring.available_space(), ring.capacity(), "Available space should equal capacity after clear");

    // Verify we can use the buffer normally after clear
    ring.push(999);
    ASSERT_EQ(ring.size(), (size_t)1, "Should be able to use buffer after clear");
    
    int value;
    ring.pop(value);
    ASSERT_EQ(value, 999, "Value should be correct after clear and reuse");

    // Test reset (should be equivalent to clear)
    for (int i = 0; i < 5; i++) {
        ring.push(i);
    }
    ring.reset();
    ASSERT_EQ(ring.size(), (size_t)0, "Buffer should be empty after reset");
    ASSERT_TRUE(ring.empty(), "Buffer should report empty after reset");

    return UNIT_TEST_SUCCESS;
}

// Test different data types
DECLARE_UNIT_TEST("ring_buffer different data types", test_ring_buffer_different_data_types) {
    // Test with different primitive types
    ring_buffer<uint8_t> byte_ring(16);
    ring_buffer<uint32_t> uint_ring(16);
    ring_buffer<uint64_t> uint64_ring(16);
    ring_buffer<char> char_ring(16);

    // Test byte operations
    for (uint8_t i = 0; i < 10; i++) {
        byte_ring.push(i);
    }
    ASSERT_EQ(byte_ring.size(), (size_t)10, "Byte ring should have 10 elements");

    uint8_t byte_val;
    byte_ring.pop(byte_val);
    ASSERT_EQ(byte_val, (uint8_t)0, "First byte should be 0");

    // Test uint32 operations
    uint_ring.push(0x12345678);
    uint_ring.push(0xABCDEF00);
    
    uint32_t uint_val;
    uint_ring.pop(uint_val);
    ASSERT_EQ(uint_val, (uint32_t)0x12345678, "uint32 value should be preserved");

    // Test uint64 operations
    uint64_ring.push(0x123456789ABCDEF0ULL);
    
    uint64_t uint64_val;
    uint64_ring.pop(uint64_val);
    ASSERT_EQ(uint64_val, (uint64_t)0x123456789ABCDEF0ULL, "uint64 value should be preserved");

    // Test char operations (important for PTY use case)
    const char* test_string = "Hello";
    for (size_t i = 0; i < strlen(test_string); i++) {
        char_ring.push(test_string[i]);
    }
    
    char result_string[10] = {0};
    for (size_t i = 0; i < strlen(test_string); i++) {
        char c;
        ASSERT_TRUE(char_ring.pop(c), "Should be able to pop character");
        result_string[i] = c;
    }
    
    ASSERT_STR_EQ(result_string, "Hello", "Character sequence should be preserved");

    return UNIT_TEST_SUCCESS;
}

// Test specialized byte ring buffer
DECLARE_UNIT_TEST("ring_buffer byte specialization", test_ring_buffer_byte_specialization) {
    byte_ring_buffer byte_ring(256);
    
    ASSERT_EQ(byte_ring.capacity(), (size_t)256, "Byte ring buffer should have correct capacity");
    
    const char* test_data = "This is a test of the byte ring buffer for PTY data!\n";
    size_t data_len = strlen(test_data);
    
    size_t written = byte_ring.write_bulk(reinterpret_cast<const uint8_t*>(test_data), data_len);
    ASSERT_EQ(written, data_len, "Should write all test data");
    ASSERT_EQ(byte_ring.size(), data_len, "Size should match written data length");
    
    uint8_t read_buffer[256] = {0};
    size_t read = byte_ring.read_bulk(read_buffer, data_len);
    ASSERT_EQ(read, data_len, "Should read all written data");
    
    for (size_t i = 0; i < data_len; i++) {
        ASSERT_EQ(read_buffer[i], (uint8_t)test_data[i], "Byte data should match exactly");
    }
    
    return UNIT_TEST_SUCCESS;
}

// Test power-of-2 capacity calculation
DECLARE_UNIT_TEST("ring_buffer power of 2 capacity", test_ring_buffer_power_of_2) {
    struct { size_t input; size_t expected; } test_cases[] = {
        {0, 1}, {1, 1}, {2, 2}, {3, 4}, {4, 4}, {5, 8}, {8, 8}, {9, 16},
        {15, 16}, {16, 16}, {17, 32}, {31, 32}, {32, 32}, {33, 64}, {100, 128}, {1024, 1024}
    };
    
    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
        ring_buffer<int> ring(test_cases[i].input);
        ASSERT_EQ(ring.capacity(), test_cases[i].expected, 
                  "Power-of-2 rounding failed for input %llu", test_cases[i].input);
    }
    
    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("ring_buffer overflow underflow", test_ring_buffer_overflow_underflow) {
    ring_buffer<int> ring(4);
    
    // Test filling to capacity
    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(ring.push(i), "Should be able to push to non-full buffer");
    }
    ASSERT_TRUE(ring.full(), "Buffer should be full");
    ASSERT_EQ(ring.size(), (size_t)4, "Size should be 4 when full");
    
    // Test overflow protection
    ASSERT_FALSE(ring.push(999), "Should not be able to push to full buffer");
    ASSERT_EQ(ring.size(), (size_t)4, "Size should remain 4 after failed push");
    
    // Test draining the buffer
    for (int i = 0; i < 4; i++) {
        int value;
        ASSERT_TRUE(ring.pop(value), "Should be able to pop from non-empty buffer");
        ASSERT_EQ(value, i, "Popped values should match insertion order");
    }
    ASSERT_TRUE(ring.empty(), "Buffer should be empty after all pops");
    
    // Test underflow protection
    int dummy;
    ASSERT_FALSE(ring.pop(dummy), "Should not be able to pop from empty buffer");
    ASSERT_EQ(ring.size(), (size_t)0, "Size should remain 0 after failed pop");
    
    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("ring_buffer clear and reset", test_ring_buffer_clear_reset) {
    ring_buffer<int> ring(8);

    // Fill the buffer
    for (int i = 0; i < 8; i++) {
        ring.push(i);
    }
    ASSERT_EQ(ring.size(), (size_t)8, "Buffer should be full");
    ASSERT_TRUE(ring.full(), "Buffer should report full");

    // Test clear
    ring.clear();
    ASSERT_EQ(ring.size(), (size_t)0, "Buffer should be empty after clear");
    ASSERT_TRUE(ring.empty(), "Buffer should report empty after clear");
    ASSERT_FALSE(ring.full(), "Buffer should not be full after clear");

    // Verify we can use the buffer normally after clear
    ring.push(999);
    ASSERT_EQ(ring.size(), (size_t)1, "Should be able to use buffer after clear");
    
    int value;
    ring.pop(value);
    ASSERT_EQ(value, 999, "Value should be correct after clear and reuse");

    // Test reset (should be equivalent to clear)
    for (int i = 0; i < 5; i++) {
        ring.push(i);
    }
    ring.reset();
    ASSERT_EQ(ring.size(), (size_t)0, "Buffer should be empty after reset");
    ASSERT_TRUE(ring.empty(), "Buffer should report empty after reset");

    return UNIT_TEST_SUCCESS;
}

// Test concurrent access patterns (basic test without actual threading)
DECLARE_UNIT_TEST("ring_buffer concurrent patterns", test_ring_buffer_concurrent_patterns) {
    ring_buffer<int> ring(32);
    
    // Simulate producer/consumer pattern
    const int num_items = 100;
    int produced = 0;
    int consumed = 0;
    
    // Alternate between producing and consuming
    while (consumed < num_items) {
        // Producer phase: try to add items if there's space
        while (produced < num_items && !ring.full()) {
            ring.push(produced);
            produced++;
        }
        
        // Consumer phase: try to consume items if available
        while (!ring.empty() && consumed < num_items) {
            int value;
            ASSERT_TRUE(ring.pop(value), "Should be able to pop when not empty");
            ASSERT_EQ(value, consumed, "Consumed value should match expected order");
            consumed++;
        }
    }
    
    ASSERT_EQ(produced, num_items, "Should have produced all items");
    ASSERT_EQ(consumed, num_items, "Should have consumed all items");
    ASSERT_TRUE(ring.empty(), "Ring should be empty after consuming all items");
    
    return UNIT_TEST_SUCCESS;
}
