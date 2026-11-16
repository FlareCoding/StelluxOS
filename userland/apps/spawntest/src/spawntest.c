#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define SELF_PATH        "/bin/spawntest"
#define OUTPUT_PATH      "/tmp/spawntest.out"
#define REFUSED_PATH     "/tmp/spawntest.refused"
#define GREETING         "spawned"
#define FILLED_SLOT      5
#define WATCHDOG_SECONDS 60

// Past the kernel's default limit of 1024 descriptors per process
#define SLOT_PAST_DESCRIPTOR_LIMIT 4096

// Every descriptor this program holds sits below this, the handles of its children included
#define OPEN_FD_SCAN_LIMIT 64
#define REAPED_CHILDREN  3
#define FIRST_EXIT_CODE  10
#define US_PER_MS        1000

// Long enough that each child still runs when its check acts on it
#define RUNNING_CHILD_MS 2000
#define KILLED_CHILD_MS  10000

// Long enough for another thread to block in waitpid before the signal or the cancel arrives
#define SIGNAL_DELAY_MS  200

// One of two threads waiting for the same child
struct shared_wait {
    pid_t pid;
    pid_t waited;
    int error;
};

extern char** environ;

static atomic_int passed;
static atomic_int failed;
static _Atomic(const char*) blocking_check;
static volatile sig_atomic_t usr1_count = 0;
static char* true_args[] = { "true", NULL };

static void check(const char* name, int cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

// Names the check about to block, which the watchdog reports if it never returns
static void watch(const char* name) {
    atomic_store(&blocking_check, name);
}

// Ends a run stuck in a blocking check with a FAIL line and the summary, since a lost wakeup would hang it silently
static void* end_hung_run(void* unused) {
    (void)unused;
    sleep(WATCHDOG_SECONDS);

    const char* name = atomic_load(&blocking_check);
    printf("  FAIL: %s (never returned)\n", name ? name : "setup");
    printf("spawntest: %d passed, %d failed\n", atomic_load(&passed), atomic_load(&failed) + 1);
    _exit(1);
}

static void count_usr1(int sig) {
    (void)sig;
    usr1_count++;
}

static int write_text(int fd, const char* text) {
    ssize_t length = (ssize_t)strlen(text);
    return write(fd, text, length) == length ? 0 : 1;
}

// Writes a line to stdout, then one to stderr, so a file both streams write to holds them in that order
static int print_to_both_streams(void) {
    if (write_text(STDOUT_FILENO, "out\n") != 0) {
        return 1;
    }

    return write_text(STDERR_FILENO, "err\n");
}

// Runs the child mode argv[1] names and returns an exit code that reports what the child received
static int run_child_mode(int argc, char** argv) {
    const char* mode = argv[1];
    const char* value = argc >= 3 ? argv[2] : "";

    if (strcmp(mode, "--exit") == 0) {
        return atoi(value);
    }

    if (strcmp(mode, "--print") == 0) {
        return write_text(STDOUT_FILENO, value);
    }

    if (strcmp(mode, "--print-both") == 0) {
        return print_to_both_streams();
    }

    if (strcmp(mode, "--expect-closed") == 0) {
        return fcntl(atoi(value), F_GETFD) < 0 && errno == EBADF ? 0 : 1;
    }

    if (strcmp(mode, "--expect-open") == 0) {
        return fcntl(atoi(value), F_GETFD) >= 0 ? 0 : 1;
    }

    if (strcmp(mode, "--expect-own-group") == 0) {
        return getpgrp() == getpid() ? 0 : 1;
    }

    if (strcmp(mode, "--sleep-ms") == 0) {
        return usleep((useconds_t)atoi(value) * US_PER_MS) == 0 ? 0 : 1;
    }

    return 1;
}

// Waits for `pid` and returns its exit code, or -1 when the wait fails or the child did not exit
static int exit_code_of(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

static int spawn_self(char* const args[], const posix_spawn_file_actions_t* actions, pid_t* pid) {
    return posix_spawn(pid, SELF_PATH, actions, NULL, args, environ);
}

// Runs a program to its end and returns its exit code, or -1
static int run(const char* path, char* const args[], const posix_spawn_file_actions_t* actions) {
    pid_t pid = 0;
    if (posix_spawn(&pid, path, actions, NULL, args, environ) != 0) {
        return -1;
    }

    return exit_code_of(pid);
}

// Returns posix_spawn's error, reaping the child when it starts anyway
static int spawn_error(const char* path, char* const args[], const posix_spawn_file_actions_t* actions,
                       const posix_spawnattr_t* attr) {
    pid_t pid = 0;
    int error = posix_spawn(&pid, path, actions, attr, args, environ);
    if (error == 0) {
        exit_code_of(pid);
    }

    return error;
}

// Spawns a child that runs for `run_ms` and returns its pid, or -1
static pid_t spawn_running_child(int run_ms) {
    char ms_text[16];
    snprintf(ms_text, sizeof(ms_text), "%d", run_ms);
    char* args[] = { "spawntest", "--sleep-ms", ms_text, NULL };

    pid_t pid = 0;
    return spawn_self(args, NULL, &pid) == 0 ? pid : -1;
}

// The descriptor the next open would take
static int lowest_free_fd(void) {
    int fd = dup(STDIN_FILENO);
    close(fd);
    return fd;
}

// Counts the descriptors this program holds, which a descriptor left open raises
static int count_open_fds(void) {
    int count = 0;
    for (int fd = 0; fd < OPEN_FD_SCAN_LIMIT; fd++) {
        count += fcntl(fd, F_GETFD) >= 0;
    }

    return count;
}

static void read_all(int fd, char* text, size_t size) {
    size_t length = 0;
    ssize_t count = 0;
    while (length + 1 < size && (count = read(fd, text + length, size - 1 - length)) > 0) {
        length += (size_t)count;
    }

    text[length] = '\0';
}

static void test_spawning(void) {
    char* false_args[] = { "false", NULL };
    check("posix_spawn runs /bin/true, which exits 0", run("/bin/true", true_args, NULL) == 0);
    check("posix_spawn runs /bin/false, which exits 1", run("/bin/false", false_args, NULL) == 1);
    check("a missing program fails with ENOENT", spawn_error("/bin/no-such-program", true_args, NULL, NULL) == ENOENT);

    char* saved_path = strdup(getenv("PATH") ? getenv("PATH") : "");
    setenv("PATH", "/no-such-dir:/bin", 1);

    pid_t pid = 0;
    int error = posix_spawnp(&pid, "true", NULL, NULL, true_args, environ);
    check("posix_spawnp finds true past a PATH directory without it", error == 0 && exit_code_of(pid) == 0);

    error = posix_spawnp(&pid, "no-such-program", NULL, NULL, true_args, environ);
    check("posix_spawnp of a name in no PATH directory fails with ENOENT", error == ENOENT);

    setenv("PATH", saved_path, 1);
    free(saved_path);
}

static void test_stdout_through_pipe(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        check("a pipe for the child's stdout", 0);
        return;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addclose(&actions, fds[0]);

    char* args[] = { "spawntest", "--print", GREETING, NULL };
    pid_t pid = 0;
    int error = spawn_self(args, &actions, &pid);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);

    char text[64] = "";
    if (error == 0) {
        read_all(fds[0], text, sizeof(text));
    }

    close(fds[0]);
    check("adddup2 sends the child's stdout into a pipe",
          error == 0 && exit_code_of(pid) == 0 && strcmp(text, GREETING) == 0);
}

static void test_output_to_file(void) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, OUTPUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    char* args[] = { "spawntest", "--print-both", NULL };
    int open_before = count_open_fds();
    int code = run(SELF_PATH, args, &actions);
    posix_spawn_file_actions_destroy(&actions);
    check("the file addopen opened is closed in the creator", count_open_fds() == open_before);

    char text[64] = "";
    int fd = open(OUTPUT_PATH, O_RDONLY);
    if (fd >= 0) {
        read_all(fd, text, sizeof(text));
        close(fd);
    }

    check("addopen sends stdout to a file and adddup2(1, 2) sends stderr after it",
          code == 0 && strcmp(text, "out\nerr\n") == 0);
}

static void test_closing_slots(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        check("a pipe for the descriptor checks", 0);
        return;
    }

    char slot_text[16];
    snprintf(slot_text, sizeof(slot_text), "%d", FILLED_SLOT);
    char* filled_args[] = { "spawntest", "--expect-closed", slot_text, NULL };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[0], FILLED_SLOT);
    posix_spawn_file_actions_addclose(&actions, FILLED_SLOT);
    check("a slot an earlier action filled can be closed again", run(SELF_PATH, filled_args, &actions) == 0);
    posix_spawn_file_actions_destroy(&actions);

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, STDIN_FILENO);
    check("closing a descriptor the child inherits fails with ENOTSUP",
          spawn_error("/bin/true", true_args, &actions, NULL) == ENOTSUP);
    posix_spawn_file_actions_destroy(&actions);

    int open_before = count_open_fds();
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[0], SLOT_PAST_DESCRIPTOR_LIMIT);
    check("a dup2 to a slot past the descriptor limit fails with EBADF",
          spawn_error("/bin/true", true_args, &actions, NULL) == EBADF);
    check("a spawn refused once the child exists leaves no descriptor behind", count_open_fds() == open_before);
    posix_spawn_file_actions_destroy(&actions);

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[0], FILLED_SLOT);
    check("a dup2 from a slot an earlier action closed fails with EBADF",
          spawn_error("/bin/true", true_args, &actions, NULL) == EBADF);
    posix_spawn_file_actions_destroy(&actions);

    // The open's file takes the closed descriptor's number, which a source checked only at install would accept
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, FILLED_SLOT + 1, SELF_PATH, O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, lowest_free_fd(), FILLED_SLOT);
    check("a dup2 from a descriptor closed before the spawn fails with EBADF",
          spawn_error("/bin/true", true_args, &actions, NULL) == EBADF);
    posix_spawn_file_actions_destroy(&actions);

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, "/");
    check("a working directory action fails with ENOTSUP",
          spawn_error("/bin/true", true_args, &actions, NULL) == ENOTSUP);
    posix_spawn_file_actions_destroy(&actions);

    unlink(REFUSED_PATH);
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, FILLED_SLOT, REFUSED_PATH, O_WRONLY | O_CREAT, 0644);
    posix_spawn_file_actions_addchdir_np(&actions, "/");

    int error = spawn_error("/bin/true", true_args, &actions, NULL);
    check("a refused request opens no file", error == ENOTSUP && access(REFUSED_PATH, F_OK) != 0);
    posix_spawn_file_actions_destroy(&actions);

    char fd_text[16];
    snprintf(fd_text, sizeof(fd_text), "%d", fds[0]);
    char* unnamed_args[] = { "spawntest", "--expect-closed", fd_text, NULL };
    check("a descriptor no action names stays out of the child", run(SELF_PATH, unnamed_args, NULL) == 0);

    close(fds[0]);
    close(fds[1]);
}

static void test_dup2_onto_itself(void) {
    int fd = open(SELF_PATH, O_RDONLY | O_CLOEXEC);
    char fd_text[16];
    snprintf(fd_text, sizeof(fd_text), "%d", fd);
    char* args[] = { "spawntest", "--expect-open", fd_text, NULL };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fd, fd);
    check("dup2 of a close-on-exec descriptor onto itself gives it to the child",
          fd >= 0 && run(SELF_PATH, args, &actions) == 0);
    posix_spawn_file_actions_destroy(&actions);

    close(fd);
}

static void test_handle_placement(void) {
    int saved_stdin = dup(STDIN_FILENO);
    close(STDIN_FILENO);

    pid_t running = spawn_running_child(RUNNING_CHILD_MS);
    int reopened = open(SELF_PATH, O_RDONLY);
    check("a spawn after closing stdin leaves descriptor 0 free", running > 0 && reopened == STDIN_FILENO);

    // Putting stdin back closes whatever descriptor 0 holds, the child's handle if it sat there
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    if (reopened > STDIN_FILENO) {
        close(reopened);
    }

    watch("a waitpid after stdin is put back");
    int status = 0;
    pid_t waited = running > 0 ? waitpid(running, &status, 0) : -1;
    check("putting stdin back before the wait still reaps the child",
          waited == running && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_attributes(void) {
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    sigset_t signals;
    sigfillset(&signals);
    posix_spawnattr_setsigdefault(&attr, &signals);
    sigemptyset(&signals);
    posix_spawnattr_setsigmask(&attr, &signals);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_RESETIDS |
                                    POSIX_SPAWN_USEVFORK);

    pid_t pid = 0;
    int error = posix_spawn(&pid, "/bin/true", NULL, &attr, true_args, environ);
    check("default signal actions, an empty signal mask, RESETIDS and USEVFORK are accepted",
          error == 0 && exit_code_of(pid) == 0);

    sigaddset(&signals, SIGUSR1);
    posix_spawnattr_setsigmask(&attr, &signals);
    check("a signal mask with a signal in it fails with ENOTSUP",
          spawn_error("/bin/true", true_args, NULL, &attr) == ENOTSUP);

    char* group_args[] = { "spawntest", "--expect-own-group", NULL };
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    error = posix_spawn(&pid, SELF_PATH, NULL, &attr, group_args, environ);
    check("SETPGROUP with group 0 puts the child in a group of its own", error == 0 && exit_code_of(pid) == 0);

    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    check("a new session fails with ENOTSUP", spawn_error("/bin/true", true_args, NULL, &attr) == ENOTSUP);

    posix_spawnattr_destroy(&attr);
}

static void test_inherited_signal_mask(void) {
    sigset_t usr1;
    sigset_t saved_mask;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, &saved_mask);

    int spawn_result = spawn_error("/bin/true", true_args, NULL, NULL);
    int system_status = system("/bin/true");
    int system_error = errno;
    FILE* output = popen("/bin/true", "r");
    int popen_error = errno;
    if (output) {
        pclose(output);
    }

    pthread_sigmask(SIG_SETMASK, &saved_mask, NULL);

    check("a spawn from a thread that blocks a signal fails with ENOTSUP", spawn_result == ENOTSUP);
    check("system from a thread that blocks a signal fails with ENOTSUP",
          system_status == -1 && system_error == ENOTSUP);
    check("popen from a thread that blocks a signal fails with ENOTSUP", !output && popen_error == ENOTSUP);
}

static void test_wait_for_one_child(void) {
    pid_t pid = spawn_running_child(RUNNING_CHILD_MS);
    int status = 0;
    check("waitpid with WNOHANG returns 0 while the child runs", pid > 0 && waitpid(pid, &status, WNOHANG) == 0);

    watch("a blocking waitpid on a running child");
    pid_t waited = pid > 0 ? waitpid(pid, &status, 0) : -1;
    check("a blocking waitpid then reaps the child", waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    check("a second waitpid on the same pid fails with ECHILD", waitpid(pid, &status, 0) == -1 && errno == ECHILD);

    char* exit_args[] = { "spawntest", "--exit", "0", NULL };
    pid = 0;
    int error = spawn_self(exit_args, NULL, &pid);
    watch("a waitpid with WUNTRACED and WCONTINUED");
    waited = error == 0 ? waitpid(pid, &status, WUNTRACED | WCONTINUED) : -1;
    check("waitpid with WUNTRACED and WCONTINUED reaps an exiting child", waited == pid && WIFEXITED(status));
}

// Reaps the children through waitpid(-1) and returns how many were reaped once, each with its expected exit code
static int reap_children_in_any_order(pid_t* pids, int count) {
    int reaped = 0;
    for (int round = 0; round < count; round++) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        for (int i = 0; i < count; i++) {
            if (pid > 0 && pids[i] == pid && WIFEXITED(status) && WEXITSTATUS(status) == FIRST_EXIT_CODE + i) {
                pids[i] = 0;
                reaped++;
            }
        }
    }

    return reaped;
}

static void test_wait_for_any_child(void) {
    pid_t pids[REAPED_CHILDREN];
    for (int i = 0; i < REAPED_CHILDREN; i++) {
        char code_text[16];
        snprintf(code_text, sizeof(code_text), "%d", FIRST_EXIT_CODE + i);
        char* args[] = { "spawntest", "--exit", code_text, NULL };
        if (spawn_self(args, NULL, &pids[i]) != 0) {
            pids[i] = -1;
        }
    }

    watch("waitpid(-1) on exiting children");
    check("waitpid(-1) reaps every child once, each with its own exit code",
          reap_children_in_any_order(pids, REAPED_CHILDREN) == REAPED_CHILDREN);
    check("wait with no children left fails with ECHILD", wait(NULL) == -1 && errno == ECHILD);

    pid_t running = spawn_running_child(RUNNING_CHILD_MS);
    check("waitpid(-1) with WNOHANG returns 0 while every child runs",
          running > 0 && waitpid(-1, NULL, WNOHANG) == 0);

    char* exiting_args[] = { "spawntest", "--exit", "0", NULL };
    pid_t exiting = 0;
    int error = spawn_self(exiting_args, NULL, &exiting);
    watch("waitpid(-1) on a running child and an exiting one");
    check("waitpid(-1) returns the child that exits first", error == 0 && waitpid(-1, NULL, 0) == exiting);

    watch("waitpid(-1) on a running child");
    check("waitpid(-1) then reaps the running child", running > 0 && waitpid(-1, NULL, 0) == running);
}

static void test_kill_by_pid(void) {
    pid_t pid = spawn_running_child(KILLED_CHILD_MS);
    int status = 0;

    watch("a waitpid on a killed child");
    check("kill reaches a spawned child through its pid",
          pid > 0 && kill(pid, SIGKILL) == 0 && waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
          WTERMSIG(status) == SIGKILL);
}

// After a pause, sends the process a SIGUSR1 that the waiting main thread takes, since this thread blocks it
static void* interrupt_after_pause(void* unused) {
    (void)unused;

    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, NULL);

    usleep(SIGNAL_DELAY_MS * US_PER_MS);
    kill(getpid(), SIGUSR1);

    return NULL;
}

// Waits for a child that outlives a SIGUSR1 another thread sends, whose handler has SA_RESTART when `restarts` is
// set, by its pid or as any child, and returns the wait's result with the child's pid in `pid`
static pid_t wait_through_usr1(int restarts, int waits_for_any, pid_t* pid, int* status, int* error) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = count_usr1;
    action.sa_flags = restarts ? SA_RESTART : 0;
    sigaction(SIGUSR1, &action, NULL);
    usr1_count = 0;

    *pid = spawn_running_child(RUNNING_CHILD_MS);
    pthread_t interrupter;
    if (*pid <= 0 || pthread_create(&interrupter, NULL, interrupt_after_pause, NULL) != 0) {
        return 0;
    }

    pid_t waited = waitpid(waits_for_any ? -1 : *pid, status, 0);
    *error = errno;
    pthread_join(interrupter, NULL);

    return waited;
}

static void test_signals_during_wait(void) {
    pid_t pid = 0;
    int status = 0;
    int error = 0;
    int open_before = count_open_fds();

    watch("a waitpid that a signal interrupts");
    pid_t waited = wait_through_usr1(0, 0, &pid, &status, &error);
    check("a signal ends a blocking waitpid with EINTR", waited == -1 && error == EINTR && usr1_count == 1);

    watch("a waitpid after an interrupted wait");
    check("the child is still reaped after the interrupted wait, leaving no descriptor behind",
          pid > 0 && waitpid(pid, &status, 0) == pid && count_open_fds() == open_before);

    watch("a waitpid that a signal under SA_RESTART interrupts");
    waited = wait_through_usr1(1, 0, &pid, &status, &error);
    check("SA_RESTART keeps a blocking waitpid waiting until the child exits",
          pid > 0 && waited == pid && WIFEXITED(status) && usr1_count == 1);

    watch("a waitpid(-1) that a signal interrupts");
    waited = wait_through_usr1(0, 1, &pid, &status, &error);
    check("a signal ends a blocking waitpid(-1) with EINTR", waited == -1 && error == EINTR && usr1_count == 1);
    if (pid > 0) {
        exit_code_of(pid);
    }
}

static void* wait_for_shared_child(void* arg) {
    struct shared_wait* wait = arg;
    wait->waited = waitpid(wait->pid, NULL, 0);
    wait->error = errno;
    return NULL;
}

static void test_two_waits_for_one_child(void) {
    struct shared_wait first = { .pid = spawn_running_child(RUNNING_CHILD_MS) };
    struct shared_wait second = first;
    pthread_t waiter;
    int created = first.pid > 0 ? pthread_create(&waiter, NULL, wait_for_shared_child, &second) : -1;

    watch("two waitpid calls on one child");
    wait_for_shared_child(&first);
    if (created == 0) {
        pthread_join(waiter, NULL);
    }

    int first_reaped = first.waited == first.pid && second.waited == -1 && second.error == ECHILD;
    int second_reaped = second.waited == first.pid && first.waited == -1 && first.error == ECHILD;
    check("of two waits for one child, one reaps it and the other fails with ECHILD",
          first.pid > 0 && (first_reaped || second_reaped));
}

static void* wait_until_cancelled(void* arg) {
    waitpid(*(pid_t*)arg, NULL, 0);
    return NULL;
}

static void test_cancelled_wait(void) {
    int open_before = count_open_fds();
    pid_t pid = spawn_running_child(RUNNING_CHILD_MS);
    pthread_t waiter;
    void* outcome = NULL;
    if (pid > 0 && pthread_create(&waiter, NULL, wait_until_cancelled, &pid) == 0) {
        usleep(SIGNAL_DELAY_MS * US_PER_MS);
        pthread_cancel(waiter);
        pthread_join(waiter, &outcome);
    }

    watch("a waitpid after a cancelled wait");
    int status = 0;
    check("a cancelled waitpid loses no child and leaves no descriptor behind",
          outcome == PTHREAD_CANCELED && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          count_open_fds() == open_before);
}

static int reports_no_use(const struct rusage* usage) {
    return usage->ru_utime.tv_sec == 0 && usage->ru_utime.tv_usec == 0 && usage->ru_stime.tv_sec == 0 &&
           usage->ru_stime.tv_usec == 0 && usage->ru_maxrss == 0 && usage->ru_nivcsw == 0;
}

static void test_resource_use(void) {
    char* args[] = { "spawntest", "--exit", "0", NULL };
    pid_t pid = 0;
    int error = spawn_self(args, NULL, &pid);

    struct rusage usage;
    memset(&usage, 0xff, sizeof(usage));
    watch("a wait4 on an exiting child");
    pid_t waited = error == 0 ? wait4(pid, NULL, 0, &usage) : -1;
    check("wait4 reports no resource use for a child", waited == pid && reports_no_use(&usage));
}

static void test_waiting(void) {
    test_wait_for_one_child();
    test_wait_for_any_child();
    test_kill_by_pid();
    test_signals_during_wait();
    test_two_waits_for_one_child();
    test_cancelled_wait();

    test_resource_use();
    check("a wait for a process group fails with ENOTSUP", waitpid(0, NULL, WNOHANG) == -1 && errno == ENOTSUP);
    check("an unknown waitpid option fails with EINVAL", waitpid(-1, NULL, __WALL) == -1 && errno == EINVAL);
}

static void test_shell_commands(void) {
    if (access("/bin/sh", X_OK) != 0) {
        printf("  SKIP: system and popen, which run /bin/sh\n");
        return;
    }

    int status = system("/bin/false");
    check("system returns the command's exit status", WIFEXITED(status) && WEXITSTATUS(status) == 1);

    FILE* output = popen(SELF_PATH " --print " GREETING, "r");
    char text[64] = "";
    if (output && !fgets(text, sizeof(text), output)) {
        text[0] = '\0';
    }

    int closed = output ? pclose(output) : -1;
    check("popen reads the command's output and pclose returns its status", strcmp(text, GREETING) == 0 && closed == 0);
}

int main(int argc, char** argv) {
    if (argc >= 2) {
        return run_child_mode(argc, argv);
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("spawntest: running posix_spawn and waitpid tests\n");

    // The watchdog starts with every signal blocked, so the interrupt checks' SIGUSR1 goes to the main thread
    sigset_t all_signals;
    sigset_t main_mask;
    sigfillset(&all_signals);
    pthread_sigmask(SIG_BLOCK, &all_signals, &main_mask);
    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, end_hung_run, NULL) == 0) {
        pthread_detach(watchdog);
    }

    pthread_sigmask(SIG_SETMASK, &main_mask, NULL);

    test_spawning();
    test_stdout_through_pipe();
    test_output_to_file();
    test_closing_slots();
    test_dup2_onto_itself();
    test_handle_placement();

    test_attributes();
    test_inherited_signal_mask();
    test_waiting();
    test_shell_commands();

    unlink(OUTPUT_PATH);
    printf("spawntest: %d passed, %d failed\n", atomic_load(&passed), atomic_load(&failed));
    return atomic_load(&failed) > 0 ? 1 : 0;
}
