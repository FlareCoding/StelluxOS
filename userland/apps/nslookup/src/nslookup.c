#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* The resolver reads its servers from this file, nslookup only reports the first */
static int first_nameserver(char* out, size_t size) {
    FILE* f = fopen("/etc/resolv.conf", "r");
    if (!f) {
        return -1;
    }

    char line[128];
    char server[64];
    int rc = -1;
    while (rc != 0 && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "nameserver %63s", server) == 1) {
            snprintf(out, size, "%s", server);
            rc = 0;
        }
    }

    fclose(f);
    return rc;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: nslookup <hostname>\r\n");
        return 1;
    }

    const char* hostname = argv[1];
    char server[64];
    if (first_nameserver(server, sizeof(server)) == 0) {
        printf("Server:  %s\r\n", server);
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* result = NULL;
    int rc = getaddrinfo(hostname, NULL, &hints, &result);
    if (rc != 0) {
        printf("** nslookup: can't resolve '%s': %s\r\n", hostname, gai_strerror(rc));
        return 1;
    }

    printf("Name:    %s\r\n", hostname);
    for (const struct addrinfo* ai = result; ai; ai = ai->ai_next) {
        char text[INET_ADDRSTRLEN];
        const struct sockaddr_in* sin = (const struct sockaddr_in*)ai->ai_addr;
        inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));
        printf("Address: %s\r\n", text);
    }

    freeaddrinfo(result);
    return 0;
}
