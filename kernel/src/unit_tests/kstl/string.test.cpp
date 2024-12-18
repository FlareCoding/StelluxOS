#include <unit_tests/unit_tests.h>
#include <memory/memory.h>
#include <string.h>

using namespace kstl;

// Utility function for comparisons, since we do have c_str().
static bool strings_equal(const kstl::string& s, const char* expected) {
    // We can compare length and then do a manual comparison
    size_t len = s.length();
    if (strlen(expected) != len) {
        return false;
    }
    if (strcmp(s.c_str(), expected) == 0) {
        return true;
    }
    return false;
}

// Test default constructor
DECLARE_UNIT_TEST("string default constructor", test_string_default_constructor) {
    string s;
    ASSERT_TRUE(s.empty(), "Default constructed string should be empty");
    ASSERT_EQ(s.length(), (size_t)0, "Length should be 0");
    ASSERT_TRUE(s.c_str() != nullptr, "c_str should return a valid pointer");
    ASSERT_EQ(*s.c_str(), '\0', "c_str should be an empty string");
    return UNIT_TEST_SUCCESS;
}

// Test construction from const char*
DECLARE_UNIT_TEST("string const char* constructor", test_string_from_cstr) {
    {
        // Small string that fits in SSO
        string s("Hello");
        ASSERT_EQ(s.length(), (size_t)5, "Length should be 5");
        ASSERT_TRUE(strings_equal(s, "Hello"), "String should match 'Hello'");
        ASSERT_FALSE(s.empty(), "Should not be empty");
    }
    {
        // Longer string to test capacity growth
        const char* long_str = "This is a very long string to test beyond SSO";
        string s(long_str);
        ASSERT_TRUE(strings_equal(s, long_str), "Should match the provided long string");
        ASSERT_EQ(s.length(), strlen(long_str), "Length should match");
        ASSERT_FALSE(s.empty(), "Should not be empty");
    }
    return UNIT_TEST_SUCCESS;
}

// Test copy constructor
DECLARE_UNIT_TEST("string copy constructor", test_string_copy_constructor) {
    string s("Copy me");
    string copy(s);
    ASSERT_TRUE(strings_equal(copy, "Copy me"), "Copied string should match the original");
    ASSERT_EQ(copy.length(), s.length(), "Lengths should match");
    // Modify original to ensure independence
    s = "Changed";
    ASSERT_TRUE(strings_equal(copy, "Copy me"), "Copy should remain unchanged");
    return UNIT_TEST_SUCCESS;
}

// Test move constructor
DECLARE_UNIT_TEST("string move constructor", test_string_move_constructor) {
    string s("Move this");
    string moved((string&&)s); // simulate move
    ASSERT_TRUE(strings_equal(moved, "Move this"), "Moved string should match original content");
    ASSERT_EQ(moved.length(), (size_t)9, "Length should be correct");
    ASSERT_TRUE(s.empty(), "Original should be empty after move");
    return UNIT_TEST_SUCCESS;
}

// Test copy assignment
DECLARE_UNIT_TEST("string copy assignment", test_string_copy_assignment) {
    string s("Initial");
    string s2;
    s2 = s;
    ASSERT_TRUE(strings_equal(s2, "Initial"), "Assigned string should match the original");
    s = "Modified";
    ASSERT_TRUE(strings_equal(s2, "Initial"), "Assigned copy should not change after original is modified");
    return UNIT_TEST_SUCCESS;
}

// Test operator+(const string&) concatenation
DECLARE_UNIT_TEST("string operator+ concatenation", test_string_operator_plus) {
    string s1("Hello");
    string s2(" World");
    string s3 = s1 + s2;
    ASSERT_TRUE(strings_equal(s3, "Hello World"), "Concatenation should result in 'Hello World'");
    ASSERT_TRUE(strings_equal(s1, "Hello"), "Original s1 should remain unchanged");
    ASSERT_TRUE(strings_equal(s2, " World"), "Original s2 should remain unchanged");
    return UNIT_TEST_SUCCESS;
}

// Test operator+=(const string&)
DECLARE_UNIT_TEST("string operator+=", test_string_operator_plus_equal) {
    string s("Hello");
    s += string(" World");
    ASSERT_TRUE(strings_equal(s, "Hello World"), "Should append ' World' to 'Hello'");
    s += string("!");
    ASSERT_TRUE(strings_equal(s, "Hello World!"), "Should append '!' at the end");
    return UNIT_TEST_SUCCESS;
}

// Test operator[]
DECLARE_UNIT_TEST("string operator[]", test_string_index_operator) {
    string s("Index");
    ASSERT_EQ(s[0], 'I', "Check first character");
    ASSERT_EQ(s[4], 'x', "Check last character");
    s[2] = 'd';
    ASSERT_EQ(s[2], 'd', "Character should be mutable");
    ASSERT_TRUE(strings_equal(s, "Index"), "String should now be 'Index' with replaced character");
    return UNIT_TEST_SUCCESS;
}

// Test equality and inequality
DECLARE_UNIT_TEST("string equality operators", test_string_equality) {
    string s1("Test");
    string s2("Test");
    string s3("Different");

    ASSERT_TRUE(s1 == s2, "Strings with same content should be equal");
    ASSERT_FALSE(s1 != s2, "Negation should hold");
    ASSERT_TRUE(s1 != s3, "Different strings should not be equal");
    ASSERT_FALSE(s1 == s3, "Check the opposite");
    return UNIT_TEST_SUCCESS;
}

// Test append(const char*)
DECLARE_UNIT_TEST("string append const char*", test_string_append_cstr) {
    string s("Hello");
    s.append(" World");
    ASSERT_TRUE(strings_equal(s, "Hello World"), "After append ' World'");
    s.append("!");
    ASSERT_TRUE(strings_equal(s, "Hello World!"), "After append '!'");
    return UNIT_TEST_SUCCESS;
}

// Test append(char)
DECLARE_UNIT_TEST("string append char", test_string_append_char) {
    string s("Hi");
    s.append('!');
    ASSERT_TRUE(strings_equal(s, "Hi!"), "Append '!' at the end");
    s.append('!');
    ASSERT_TRUE(strings_equal(s, "Hi!!"), "Append '!' again");
    return UNIT_TEST_SUCCESS;
}

// Test reserve and capacity
DECLARE_UNIT_TEST("string reserve and capacity", test_string_reserve) {
    string s("Small");
    size_t old_capacity = s.capacity();
    s.reserve(old_capacity + 20);
    ASSERT_TRUE(s.capacity() >= old_capacity + 20, "Capacity should increase after reserve");
    ASSERT_TRUE(strings_equal(s, "Small"), "String content should remain unchanged");
    return UNIT_TEST_SUCCESS;
}

// Test resize
DECLARE_UNIT_TEST("string resize", test_string_resize) {
    string s("Hello");
    s.resize(2);
    ASSERT_EQ(s.length(), (size_t)2, "Length should now be 2");
    ASSERT_TRUE(strings_equal(s, "He"), "Should truncate to 'He'");

    s.resize(5);
    ASSERT_EQ(s.length(), (size_t)5, "Length should be 5 now");
    // The new characters beyond current length might be '\0' or uninitialized - 
    // The specification for this is not fully defined in the provided code. 
    // We assume resizing adds '\0' padding.
    // Just check that the first two characters remain correct:
    ASSERT_EQ(s[0], 'H', "First char should still be H");
    ASSERT_EQ(s[1], 'e', "Second char should still be e");

    return UNIT_TEST_SUCCESS;
}

// Test find(char)
DECLARE_UNIT_TEST("string find(char)", test_string_find_char) {
    string s("Find me in this string!");
    size_t pos = s.find('m');
    ASSERT_EQ(pos, (size_t)5, "Character 'm' should be at index 5 (0-based: 'F'=0,'i'=1,'n'=2,'d'=3,' '=4,'m'=5)");
    pos = s.find('z');
    ASSERT_EQ(pos, string::npos, "Character 'z' not present, should return npos");
    return UNIT_TEST_SUCCESS;
}

// Test find(const char*)
DECLARE_UNIT_TEST("string find(const char*)", test_string_find_cstr) {
    string s("This is a sample string");
    size_t pos = s.find("sample");
    // "This is a sample string"
    //            ^
    // "sample" starts at index 10 (T=0,h=1,i=2,s=3,' '=4,i=5,s=6,' '=7,a=8,' '=9,s=10)
    ASSERT_EQ(pos, (size_t)10, "Should find 'sample' at index 10");
    pos = s.find("none");
    ASSERT_EQ(pos, string::npos, "Non-existent substring should return npos");
    return UNIT_TEST_SUCCESS;
}

// Test find(const string&)
DECLARE_UNIT_TEST("string find(const string&)", test_string_find_string) {
    string s("Look within this string");
    string target("within");
    size_t pos = s.find(target);
    // "Look within this string"
    //       ^ 
    // 'within' starts at index 5 ('L'=0,'o'=1,'o'=2,'k'=3,' '=4,'w'=5)
    ASSERT_EQ(pos, (size_t)5, "Should find 'within' at index 5");
    string not_found("xyz");
    ASSERT_EQ(s.find(not_found), string::npos, "Non-existent substring should return npos");
    return UNIT_TEST_SUCCESS;
}

// Test substring
DECLARE_UNIT_TEST("string substring", test_string_substring) {
    string s("Hello World");
    // Substring "Hello"
    string sub = s.substring(0, 5);
    ASSERT_TRUE(strings_equal(sub, "Hello"), "Should extract 'Hello'");

    // Substring "World"
    string sub2 = s.substring(6, 5);
    ASSERT_TRUE(strings_equal(sub2, "World"), "Should extract 'World'");

    // Substring with length = npos (till end)
    string sub3 = s.substring(6);
    ASSERT_TRUE(strings_equal(sub3, "World"), "Should extract until end if length = npos");

    // Out of range start
    string sub4 = s.substring(50);
    ASSERT_TRUE(strings_equal(sub4, ""), "Out of range start should return empty substring");
    return UNIT_TEST_SUCCESS;
}

// Test clear
DECLARE_UNIT_TEST("string clear", test_string_clear) {
    string s("Not empty");
    s.clear();
    ASSERT_TRUE(s.empty(), "Should be empty after clear");
    ASSERT_EQ(s.length(), (size_t)0, "Length should be 0");
    ASSERT_EQ(*s.c_str(), '\0', "Should be empty string");
    return UNIT_TEST_SUCCESS;
}

// Test to_string(int)
DECLARE_UNIT_TEST("string to_string(int)", test_string_to_string_int) {
    string s = to_string(12345);
    ASSERT_TRUE(strings_equal(s, "12345"), "Integer 12345 should become '12345', found %s instead", s.c_str());

    s = to_string(-987);
    ASSERT_TRUE(strings_equal(s, "-987"), "Integer -987 should become '-987', found %s instead", s.c_str());
    return UNIT_TEST_SUCCESS;
}

// Test to_string(unsigned int)
DECLARE_UNIT_TEST("string to_string(unsigned int)", test_string_to_string_uint) {
    unsigned int val = 4294967295U; // max 32-bit unsigned int
    string s = to_string(val);
    ASSERT_TRUE(strings_equal(s, "4294967295"), "Max unsigned int should convert correctly");
    return UNIT_TEST_SUCCESS;
}

// Test that c_str() and data() are consistent
DECLARE_UNIT_TEST("string c_str and data consistency", test_string_cstr_data) {
    string s("Check c_str");
    ASSERT_TRUE(s.c_str() == s.data(), "c_str and data should return the same pointer");
    ASSERT_TRUE(strcmp(s.c_str(), "Check c_str") == 0, "c_str should hold the correct string");
    return UNIT_TEST_SUCCESS;
}

// Test concatenation of empty strings
DECLARE_UNIT_TEST("string empty concatenation", test_string_empty_concatenation) {
    string empty;
    string nonempty("Hi");

    string result = empty + nonempty;
    ASSERT_TRUE(strings_equal(result, "Hi"), "Empty + 'Hi' should be 'Hi'");

    result = nonempty + empty;
    ASSERT_TRUE(strings_equal(result, "Hi"), "'Hi' + empty should be 'Hi'");

    empty += "";
    ASSERT_TRUE(empty.empty(), "Appending empty string to empty should remain empty");

    nonempty += "";
    ASSERT_TRUE(strings_equal(nonempty, "Hi"), "Appending empty string should not change original");
    return UNIT_TEST_SUCCESS;
}

// Test large string scenario
DECLARE_UNIT_TEST("string large scenario", test_string_large) {
    // Create a large string beyond SSO
    string large("This is a large string that exceeds the small buffer optimization");
    ASSERT_FALSE(large.empty(), "Should not be empty");
    ASSERT_EQ(large.length(), strlen("This is a large string that exceeds the small buffer optimization"), "Check length");

    // Append more characters to ensure capacity expansion
    for (int i = 0; i < 50; i++) {
        large.append("x");
    }

    ASSERT_EQ(
        large.length(),
        strlen("This is a large string that exceeds the small buffer optimization") + 50,
        "Check length after appending"
    );
    return UNIT_TEST_SUCCESS;
}
