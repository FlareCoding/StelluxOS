#include <stlibc/stlibc.h>
#include <stlibc/string/string.h>
#include <stlibc/string/format.h>

#include <stlibc/proc/pid.h>
#include <fs/vfs.h>

int main(int argc, char* argv[]) {
    printf("ls executed! pid: %i\n", getpid());

    return 0;
} 