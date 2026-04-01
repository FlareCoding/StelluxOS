#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    uid_t uid = getuid();

    FILE* fp = fopen("/etc/passwd", "r");
    if (!fp) {
        printf("uid %u\n", (unsigned)uid);
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* passwd format: name:pass:uid:gid:... */
        char* name_end = strchr(line, ':');
        if (!name_end) continue;

        char* pass_end = strchr(name_end + 1, ':');
        if (!pass_end) continue;

        unsigned entry_uid = (unsigned)strtoul(pass_end + 1, NULL, 10);
        if (entry_uid == (unsigned)uid) {
            *name_end = '\0';
            puts(line);
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    printf("uid %u\n", (unsigned)uid);
    return 0;
}
