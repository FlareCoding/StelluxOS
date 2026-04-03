#include <cstdio>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <typeinfo>

static int g_failures = 0;

static void report(const char* name, bool passed) {
    printf("  %-28s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) {
        g_failures++;
    }
}

// Global constructor/destructor test
static bool g_ctor_ran = false;

struct global_init_test {
    global_init_test() { g_ctor_ran = true; }
};

static global_init_test g_init_test;

// Test: heap allocation
static bool test_heap() {
    int* p = new int(42);
    bool ok = (p != nullptr && *p == 42);
    delete p;

    int* arr = new int[10];
    for (int i = 0; i < 10; i++) {
        arr[i] = i * i;
    }
    ok = ok && (arr[9] == 81);
    delete[] arr;

    return ok;
}

// Test: std::vector
static bool test_vector() {
    std::vector<int> v;
    for (int i = 0; i < 100; i++) {
        v.push_back(i);
    }
    if (v.size() != 100) return false;
    if (v[0] != 0 || v[99] != 99) return false;

    int sum = 0;
    for (auto val : v) {
        sum += val;
    }
    // sum of 0..99 = 4950
    return sum == 4950;
}

// Test: std::string
static bool test_string() {
    std::string hello = "Hello";
    std::string world = " World";
    std::string combined = hello + world;

    if (combined != "Hello World") return false;
    if (combined.size() != 11) return false;
    if (combined.find("World") != 6) return false;

    // Test with longer string (forces heap allocation in most implementations)
    std::string long_str(256, 'x');
    if (long_str.size() != 256) return false;
    if (long_str[0] != 'x' || long_str[255] != 'x') return false;

    return true;
}

// Test: std::unique_ptr
static bool test_unique_ptr() {
    auto p = std::make_unique<int>(99);
    if (!p || *p != 99) return false;

    auto p2 = std::move(p);
    if (p != nullptr) return false;
    if (!p2 || *p2 != 99) return false;

    // unique_ptr to array
    auto arr = std::make_unique<int[]>(10);
    for (int i = 0; i < 10; i++) {
        arr[i] = i;
    }
    if (arr[5] != 5) return false;

    return true;
}

// Test: exception handling
static bool test_exceptions() {
    bool caught = false;
    try {
        throw std::runtime_error("test exception");
    } catch (const std::runtime_error& e) {
        caught = true;
        std::string msg = e.what();
        if (msg != "test exception") return false;
    } catch (...) {
        return false;
    }
    if (!caught) return false;

    // Test integer throw/catch
    caught = false;
    try {
        throw 42;
    } catch (int val) {
        caught = (val == 42);
    }
    if (!caught) return false;

    // Test that non-throwing code doesn't catch
    bool no_throw_ok = true;
    try {
        int x = 1 + 1;
        (void)x;
    } catch (...) {
        no_throw_ok = false;
    }

    return no_throw_ok;
}

// Test: RTTI / dynamic_cast
struct base {
    virtual ~base() = default;
    virtual int id() { return 0; }
};

struct derived : public base {
    int id() override { return 1; }
    int extra() { return 42; }
};

static bool test_rtti() {
    std::unique_ptr<base> b = std::make_unique<derived>();

    // dynamic_cast should succeed
    derived* d = dynamic_cast<derived*>(b.get());
    if (!d) return false;
    if (d->extra() != 42) return false;
    if (d->id() != 1) return false;

    // dynamic_cast to wrong type should return nullptr
    struct other : public base {
        int id() override { return 2; }
    };
    other* o = dynamic_cast<other*>(b.get());
    if (o != nullptr) return false;

    // typeid
    base& ref = *b;
    if (typeid(ref) != typeid(derived)) return false;

    return true;
}

// Test: std::sort
static bool test_sort() {
    std::vector<int> v = {5, 3, 1, 4, 2, 8, 7, 6, 9, 0};
    std::sort(v.begin(), v.end());

    for (int i = 0; i < 10; i++) {
        if (v[static_cast<size_t>(i)] != i) return false;
    }

    // Reverse sort
    std::sort(v.begin(), v.end(), std::greater<int>());
    if (v[0] != 9 || v[9] != 0) return false;

    return true;
}

// Test: templates
template<typename T>
T max_of(T a, T b) {
    return (a > b) ? a : b;
}

template<typename T, typename... Args>
T sum_all(T first, Args... rest) {
    if constexpr (sizeof...(rest) == 0) {
        return first;
    } else {
        return first + sum_all(rest...);
    }
}

static bool test_templates() {
    if (max_of(3, 7) != 7) return false;
    if (max_of(std::string("apple"), std::string("banana")) != "banana") return false;

    // Variadic template
    if (sum_all(1, 2, 3, 4, 5) != 15) return false;

    return true;
}

// Test: lambdas
static bool test_lambdas() {
    // Simple lambda
    auto add = [](int a, int b) { return a + b; };
    if (add(3, 4) != 7) return false;

    // Capturing lambda
    int x = 10;
    auto add_x = [&x](int a) { return a + x; };
    if (add_x(5) != 15) return false;

    // Lambda with std::algorithm
    std::vector<int> v = {1, 2, 3, 4, 5};
    int sum = 0;
    std::for_each(v.begin(), v.end(), [&sum](int val) { sum += val; });
    if (sum != 15) return false;

    // Mutable lambda
    int counter = 0;
    auto inc = [counter]() mutable { return ++counter; };
    if (inc() != 1) return false;
    if (inc() != 2) return false;
    if (counter != 0) return false; // original unchanged

    return true;
}

int main() {
    printf("\n");
    printf("Stellux C++ Runtime Test\n");
    printf("========================\n");

    report("Global constructor",  g_ctor_ran);
    report("Heap allocation",     test_heap());
    report("std::vector",         test_vector());
    report("std::string",         test_string());
    report("std::unique_ptr",     test_unique_ptr());
    report("Exception handling",  test_exceptions());
    report("RTTI / dynamic_cast", test_rtti());
    report("std::sort",           test_sort());
    report("Templates",           test_templates());
    report("Lambdas",             test_lambdas());

    printf("========================\n");
    if (g_failures == 0) {
        printf("All tests passed.\n");
    } else {
        printf("%d test(s) FAILED.\n", g_failures);
    }
    printf("\n");

    return g_failures;
}
