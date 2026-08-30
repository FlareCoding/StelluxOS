#include "screen.hpp"
#include "server.hpp"

#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <vector>

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    screen scr;
    if (scr.init() != 0) {
        printf("stlxdm: no framebuffer, exiting\r\n");
        return 1;
    }

    /* Composition happens here, never in the scanout mapping */
    uint8_t* backbuffer =
        static_cast<uint8_t*>(malloc((size_t)scr.width * scr.height * 4));
    if (!backbuffer) {
        printf("stlxdm: backbuffer allocation failed\r\n");
        scr.shutdown();
        return 1;
    }

    server srv;
    if (srv.init(&scr) != 0) {
        printf("stlxdm: failed to bind %s\r\n", SWP_SOCKET_PATH);
        scr.shutdown();
        return 1;
    }

    printf("stlxdm: serving %ux%u\r\n", scr.width, scr.height);

    std::vector<pollfd> fds;
    while (true) {
        fds.clear();
        srv.collect_fds(fds);

        int rc = poll(fds.data(), fds.size(), -1);
        if (rc < 0) {
            continue;
        }

        srv.pump(fds);
        srv.present(scr, backbuffer);
    }
}
