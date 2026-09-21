/* isoprobe: verifies that a user process cannot read the kernel half of
 * the address space. Kernel unit tests never run at user privilege, so
 * this is the only check that exercises that boundary.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stlx/proc.h>

#define KERNEL_IMAGE_BASE 0xffffffff80000000UL
#define SELF_PATH "/bin/isoprobe"
#define CHILD_ARG "--probe"

/* A user fault kills the process instead of reaching a signal handler,
 * so the read runs in a child and the parent judges how it ended */
static int run_child(void) {
    volatile unsigned long* probe = (volatile unsigned long*)KERNEL_IMAGE_BASE;
    unsigned long value = *probe;

    printf("isoprobe: read 0x%lx from user mode: %016lx\n",
           KERNEL_IMAGE_BASE, value);
    return 1;
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], CHILD_ARG) == 0) {
            return run_child();
        }

        printf("usage: isoprobe\n");
        return 2;
    }

    /* proc_create receives the arguments after argv[0] */
    const char* child_argv[] = { CHILD_ARG, NULL };
    int handle = proc_exec(SELF_PATH, child_argv);
    if (handle < 0) {
        printf("isoprobe: failed to spawn the probe child\n");
        return 2;
    }

    int status = 0;
    if (proc_wait(handle, &status) != 0) {
        printf("isoprobe: failed to wait for the probe child\n");
        return 2;
    }

    if (STLX_WIFSIGNALED(status) && STLX_WTERMSIG(status) == SIGSEGV) {
        printf("isoprobe: kernel half unreadable from user mode: PASS\n");
        printf("isoprobe: 1 passed\n");
        return 0;
    }

    if (STLX_WIFEXITED(status)) {
        printf("isoprobe: kernel half unreadable from user mode: FAIL\n");
    } else {
        printf("isoprobe: probe child died from signal %d, inconclusive\n",
               STLX_WTERMSIG(status));
    }
    printf("isoprobe: 0 passed, 1 failed\n");
    return 1;
}
