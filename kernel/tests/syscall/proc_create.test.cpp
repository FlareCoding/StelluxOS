#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_proc.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "resource/providers/proc_provider.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/uaccess.h"
#include "mm/vma.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"

TEST_SUITE(proc_create_syscall);

using test_helpers::user_space_scope;

// A summary of the strings on a process's initial stack, as its C runtime reads them
struct startup_strings {
    uint64_t argc;
    uint64_t envc;
    size_t   last_arg_length;
    size_t   last_env_length;
};

static const char PROGRAM[] = "/bin/hello";

// The user area holds a pointer list at its start, the program path on the page after it, and a run of letters ending
// in a NUL on the pages after that, so a pointer into the run gives a string of any length up to `RUN_LETTERS`
constexpr size_t LIST_BYTES  = (sched::MAX_ARG_STRINGS + 2) * sizeof(uint64_t);
constexpr size_t PATH_OFFSET = pmm::page_align_up(LIST_BYTES);
constexpr size_t RUN_OFFSET  = PATH_OFFSET + pmm::PAGE_SIZE;
constexpr size_t RUN_LETTERS = sched::MAX_ARG_STRLEN;
constexpr size_t AREA_BYTES  = RUN_OFFSET + pmm::page_align_up(RUN_LETTERS + 1);

// Two strings of this many letters fill an array to its last byte
constexpr size_t HALF_ARRAY_LETTERS = sched::MAX_ARG_ARRAY_BYTES / 2 - 1;

constexpr size_t FILL_CHUNK_BYTES = 512;

static mm::mm_context* g_ctx;
static uintptr_t g_area;
static char* g_string_copy;

// Copies into the area, under the `user_space_scope` on `g_ctx` that the caller must hold
static bool copy_to_area(size_t offset, const void* src, size_t len) {
    return mm::uaccess::copy_to_user(reinterpret_cast<void*>(g_area + offset), src, len) == mm::uaccess::OK;
}

static bool write_path_and_run() {
    char letters[FILL_CHUNK_BYTES];
    string::memset(letters, 'a', sizeof(letters));

    user_space_scope scope(g_ctx);
    if (!copy_to_area(PATH_OFFSET, PROGRAM, sizeof(PROGRAM))) {
        return false;
    }

    for (size_t done = 0; done < RUN_LETTERS; done += sizeof(letters)) {
        size_t chunk = RUN_LETTERS - done < sizeof(letters) ? RUN_LETTERS - done : sizeof(letters);
        if (!copy_to_area(RUN_OFFSET + done, letters, chunk)) {
            return false;
        }
    }

    const char terminator = '\0';
    return copy_to_area(RUN_OFFSET + RUN_LETTERS, &terminator, 1);
}

static int32_t map_user_area() {
    g_ctx = mm::mm_context_create();
    g_string_copy = static_cast<char*>(heap::ualloc(sched::MAX_ARG_STRLEN));
    if (!g_ctx || !g_string_copy) {
        return -1;
    }

    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    uint32_t flags = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;
    if (mm::mm_context_map_anonymous(g_ctx, 0, AREA_BYTES, prot, flags, &g_area) != mm::MM_CTX_OK) {
        return -1;
    }

    return write_path_and_run() ? 0 : -1;
}

static int32_t unmap_user_area() {
    if (g_ctx) {
        mm::mm_context_release(g_ctx);
    }

    heap::ufree(g_string_copy);
    g_ctx = nullptr;
    g_string_copy = nullptr;
    return 0;
}

BEFORE_ALL(proc_create_syscall, map_user_area);
AFTER_ALL(proc_create_syscall, unmap_user_area);

// The user address of the string of `length` letters that ends where the run ends
static uint64_t letter_string(size_t length) {
    return g_area + RUN_OFFSET + RUN_LETTERS - length;
}

// Lays out `targets` and the terminator as the area's pointer list
static bool write_list(const uint64_t* targets, size_t count) {
    const uint64_t terminator = 0;

    user_space_scope scope(g_ctx);
    return copy_to_area(0, targets, count * sizeof(uint64_t)) &&
           copy_to_area(count * sizeof(uint64_t), &terminator, sizeof(terminator));
}

// Lays out `count` pointers to `target` and the terminator as the area's pointer list
static bool write_repeated_list(size_t count, uint64_t target) {
    uint64_t pointers[FILL_CHUNK_BYTES / sizeof(uint64_t)];
    for (uint64_t& pointer : pointers) {
        pointer = target;
    }

    user_space_scope scope(g_ctx);
    size_t list_bytes = count * sizeof(uint64_t);
    for (size_t done = 0; done < list_bytes; done += sizeof(pointers)) {
        size_t chunk = list_bytes - done < sizeof(pointers) ? list_bytes - done : sizeof(pointers);
        if (!copy_to_area(done, pointers, chunk)) {
            return false;
        }
    }

    const uint64_t terminator = 0;
    return copy_to_area(list_bytes, &terminator, sizeof(terminator));
}

// Calls proc_create on the program path, passing the area's pointer list or no list as argv and envp
static int64_t create_process(uint64_t u_argv, uint64_t u_envp) {
    user_space_scope scope(g_ctx);
    return sys_proc_create(g_area + PATH_OFFSET, u_argv, u_envp, 0, 0, 0);
}

// proc_create's result, closing the returned handle when there is one
static int64_t try_create_process(uint64_t u_argv, uint64_t u_envp) {
    int64_t result = create_process(u_argv, u_envp);
    if (result >= 0) {
        resource::close(sched::current(), static_cast<resource::handle_t>(result));
    }

    return result;
}

// The unstarted process that a handle from proc_create refers to, or null
static sched::task* process_of_handle(int64_t handle) {
    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(sched::current()->handles, static_cast<resource::handle_t>(handle), 0,
                                             &obj);
    if (rc != resource::HANDLE_OK) {
        return nullptr;
    }

    auto* pr = resource::proc_provider::get_proc_resource(obj);
    sched::task* process = pr ? pr->child : nullptr;
    resource::resource_release(obj);

    return process;
}

// Reads a word, or a string's length, from the address space that the caller's `user_space_scope` selects
static bool read_word(uintptr_t va, uint64_t* out) {
    return mm::uaccess::copy_from_user(out, reinterpret_cast<const void*>(va), sizeof(*out)) == mm::uaccess::OK;
}

static bool read_string_length(uint64_t va, size_t* out) {
    int32_t rc = mm::uaccess::copy_cstr_from_user(g_string_copy, sched::MAX_ARG_STRLEN,
                                                  reinterpret_cast<const char*>(va));
    if (rc != mm::uaccess::OK) {
        return false;
    }

    *out = string::strnlen(g_string_copy, sched::MAX_ARG_STRLEN);
    return true;
}

// Reads the initial stack of the process `handle` refers to as its C runtime would, through its address space
static bool read_startup_strings(int64_t handle, startup_strings* out) {
    sched::task* process = process_of_handle(handle);
    if (!process) {
        return false;
    }

    user_space_scope scope(process->exec.mm_ctx);
    if (!read_word(process->exec.task_stack_top, &out->argc) || out->argc == 0) {
        return false;
    }

    uintptr_t argv = process->exec.task_stack_top + sizeof(uint64_t);
    uint64_t last_arg = 0;
    if (!read_word(argv + (out->argc - 1) * sizeof(uint64_t), &last_arg)) {
        return false;
    }

    if (!read_string_length(last_arg, &out->last_arg_length)) {
        return false;
    }

    // The environment list follows the argument list's terminator
    uintptr_t envp = argv + (out->argc + 1) * sizeof(uint64_t);
    uint64_t last_env = 0;
    for (out->envc = 0; out->envc <= sched::MAX_ARG_STRINGS; out->envc++) {
        uint64_t env = 0;
        if (!read_word(envp + out->envc * sizeof(uint64_t), &env)) {
            return false;
        }

        if (env == 0) {
            break;
        }

        last_env = env;
    }

    out->last_env_length = 0;
    return last_env == 0 || read_string_length(last_env, &out->last_env_length);
}

TEST(proc_create_syscall, the_initial_stack_holds_the_most_strings_allowed) {
    ASSERT_TRUE(write_repeated_list(sched::MAX_ARG_STRINGS, letter_string(1)));

    int64_t handle = create_process(g_area, g_area);
    ASSERT_TRUE(handle >= 0);

    startup_strings strings = {};
    EXPECT_TRUE(read_startup_strings(handle, &strings));
    resource::close(sched::current(), static_cast<resource::handle_t>(handle));

    EXPECT_EQ(strings.argc, sched::MAX_ARG_STRINGS + 1);
    EXPECT_EQ(strings.envc, sched::MAX_ARG_STRINGS);
    EXPECT_EQ(strings.last_arg_length, static_cast<size_t>(1));
    EXPECT_EQ(strings.last_env_length, static_cast<size_t>(1));
}

TEST(proc_create_syscall, one_string_too_many_fails_with_e2big) {
    ASSERT_TRUE(write_repeated_list(sched::MAX_ARG_STRINGS + 1, letter_string(1)));

    EXPECT_EQ(try_create_process(g_area, 0), syscall::E2BIG);
    EXPECT_EQ(try_create_process(0, g_area), syscall::E2BIG);
}

TEST(proc_create_syscall, the_initial_stack_holds_the_longest_string_allowed) {
    const uint64_t longest = letter_string(sched::MAX_ARG_STRLEN - 1);
    ASSERT_TRUE(write_list(&longest, 1));

    int64_t handle = create_process(g_area, g_area);
    ASSERT_TRUE(handle >= 0);

    startup_strings strings = {};
    EXPECT_TRUE(read_startup_strings(handle, &strings));
    resource::close(sched::current(), static_cast<resource::handle_t>(handle));

    EXPECT_EQ(strings.argc, static_cast<uint64_t>(2));
    EXPECT_EQ(strings.envc, static_cast<uint64_t>(1));
    EXPECT_EQ(strings.last_arg_length, sched::MAX_ARG_STRLEN - 1);
    EXPECT_EQ(strings.last_env_length, sched::MAX_ARG_STRLEN - 1);
}

TEST(proc_create_syscall, a_string_one_byte_too_long_fails_with_e2big) {
    const uint64_t too_long = letter_string(sched::MAX_ARG_STRLEN);
    ASSERT_TRUE(write_list(&too_long, 1));

    EXPECT_EQ(try_create_process(g_area, 0), syscall::E2BIG);
    EXPECT_EQ(try_create_process(0, g_area), syscall::E2BIG);
}

TEST(proc_create_syscall, a_full_array_is_accepted_and_strings_past_it_fail_with_e2big) {
    const uint64_t full[] = {letter_string(HALF_ARRAY_LETTERS), letter_string(HALF_ARRAY_LETTERS)};
    ASSERT_TRUE(write_list(full, 2));
    EXPECT_TRUE(try_create_process(g_area, 0) >= 0);

    const uint64_t one_byte_over[] = {letter_string(HALF_ARRAY_LETTERS), letter_string(HALF_ARRAY_LETTERS + 1)};
    ASSERT_TRUE(write_list(one_byte_over, 2));
    EXPECT_EQ(try_create_process(g_area, 0), syscall::E2BIG);

    const uint64_t one_string_over[] = {
        letter_string(HALF_ARRAY_LETTERS), letter_string(HALF_ARRAY_LETTERS), letter_string(0),
    };

    ASSERT_TRUE(write_list(one_string_over, 3));
    EXPECT_EQ(try_create_process(g_area, 0), syscall::E2BIG);
}
