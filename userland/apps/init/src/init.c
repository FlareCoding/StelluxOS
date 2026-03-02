#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <unistd.h>

static int run_vma_syscall_demo(void) {
    const size_t page_size = 4096;
    const size_t map_len = page_size * 2;

    uint8_t* region = (uint8_t*)mmap(
        NULL, map_len,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );
    if (region == MAP_FAILED) {
        printf("mmap failed: errno=%d (%s)\r\n", errno, strerror(errno));
        return 1;
    }

    region[0] = 0x2A;
    region[page_size] = 0x55;
    printf("mmap ok: region=%p first=0x%x second=0x%x\r\n",
           (void*)region, region[0], region[page_size]);

    if (mprotect(region, page_size, PROT_READ) != 0) {
        printf("mprotect RO failed: errno=%d (%s)\r\n", errno, strerror(errno));
        munmap(region, map_len);
        return 1;
    }
    printf("mprotect RO ok: first page is now read-only (read value=0x%x)\r\n", region[0]);

    if (mprotect(region, page_size, PROT_READ | PROT_WRITE) != 0) {
        printf("mprotect RW restore failed: errno=%d (%s)\r\n", errno, strerror(errno));
        munmap(region, map_len);
        return 1;
    }
    region[1] = 0x33;
    printf("mprotect RW restore ok: first page write value=0x%x\r\n", region[1]);

    if (munmap(region + page_size, page_size) != 0) {
        printf("munmap upper page failed: errno=%d (%s)\r\n", errno, strerror(errno));
        munmap(region, page_size);
        return 1;
    }
    printf("munmap upper page ok\r\n");

    if (munmap(region + page_size, page_size) != 0) {
        printf("munmap upper page (second call) failed: errno=%d (%s)\r\n", errno, strerror(errno));
        munmap(region, page_size);
        return 1;
    }
    printf("munmap upper page second call ok (idempotent)\r\n");

    if (munmap(region, page_size) != 0) {
        printf("munmap first page failed: errno=%d (%s)\r\n", errno, strerror(errno));
        return 1;
    }
    printf("munmap first page ok\r\n");

    return 0;
}

static int run_resource_fd_demo(void) {
    const char* path = "/resource_demo_file";
    const char* msg = "hello from resource fd demo";

    // Example 1: open/write/close + reopen/read/close
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        printf("open(O_CREAT|O_RDWR) failed: errno=%d (%s)\r\n", errno, strerror(errno));
        return 1;
    }

    ssize_t wr = write(fd, msg, strlen(msg));
    if (wr != (ssize_t)strlen(msg)) {
        printf("write failed: wrote=%ld errno=%d (%s)\r\n",
               (long)wr, errno, strerror(errno));
        close(fd);
        return 1;
    }

    if (close(fd) != 0) {
        printf("close after write failed: errno=%d (%s)\r\n", errno, strerror(errno));
        return 1;
    }

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("reopen(O_RDONLY) failed: errno=%d (%s)\r\n", errno, strerror(errno));
        return 1;
    }

    char buf[128] = {};
    ssize_t rd = read(fd, buf, sizeof(buf) - 1);
    if (rd < 0) {
        printf("read failed: errno=%d (%s)\r\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    buf[rd] = '\0';
    printf("resource fd example 1 ok: read back \"%s\" (%ld bytes)\r\n", buf, (long)rd);

    if (close(fd) != 0) {
        printf("close after read failed: errno=%d (%s)\r\n", errno, strerror(errno));
        return 1;
    }

    // Example 2: invalid handle should fail with EBADF
    errno = 0;
    rd = read(-1, buf, 1);
    if (rd != -1 || errno != EBADF) {
        printf("resource fd example 2 failed: read(-1) => %ld errno=%d\r\n",
               (long)rd, errno);
        return 1;
    }
    printf("resource fd example 2 ok: read(-1) -> EBADF\r\n");

    return 0;
}

static int run_socket_ipc_demo(void) {
    const char* socket_path = "/uds_init_demo";
    const char* client_msg = "client->server";
    const char* server_msg = "server->client";

    int server_fd = -1;
    int client_fd = -1;
    int accepted_fd = -1;
    int rc = 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        printf("socket demo path too long\r\n");
        return 1;
    }
    memcpy(addr.sun_path, socket_path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("socket(server) failed: errno=%d (%s)\r\n", errno, strerror(errno));
        goto done;
    }

    if (bind(server_fd, (const struct sockaddr*)&addr, addr_len) != 0) {
        printf("bind failed: errno=%d (%s)\r\n", errno, strerror(errno));
        goto done;
    }

    if (listen(server_fd, 4) != 0) {
        printf("listen failed: errno=%d (%s)\r\n", errno, strerror(errno));
        goto done;
    }

    client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
        printf("socket(client) failed: errno=%d (%s)\r\n", errno, strerror(errno));
        goto done;
    }

    if (connect(client_fd, (const struct sockaddr*)&addr, addr_len) != 0) {
        printf("connect failed: errno=%d (%s)\r\n", errno, strerror(errno));
        goto done;
    }

    accepted_fd = accept(server_fd, NULL, NULL);
    if (accepted_fd < 0) {
        printf("accept failed: errno=%d (%s)\r\n", errno, strerror(errno));
        goto done;
    }

    ssize_t wr = sendto(client_fd, client_msg, strlen(client_msg), 0, NULL, 0);
    if (wr != (ssize_t)strlen(client_msg)) {
        printf("sendto(client) failed: wrote=%ld errno=%d (%s)\r\n",
               (long)wr, errno, strerror(errno));
        goto done;
    }

    char recv_buf[64] = {};
    ssize_t rd = recvfrom(accepted_fd, recv_buf, sizeof(recv_buf) - 1, 0, NULL, NULL);
    if (rd != (ssize_t)strlen(client_msg)) {
        printf("recvfrom(server) failed: read=%ld errno=%d (%s)\r\n",
               (long)rd, errno, strerror(errno));
        goto done;
    }
    recv_buf[rd] = '\0';
    if (strcmp(recv_buf, client_msg) != 0) {
        printf("recvfrom(server) mismatch: got=\"%s\"\r\n", recv_buf);
        goto done;
    }

    wr = sendto(accepted_fd, server_msg, strlen(server_msg), 0, NULL, 0);
    if (wr != (ssize_t)strlen(server_msg)) {
        printf("sendto(server) failed: wrote=%ld errno=%d (%s)\r\n",
               (long)wr, errno, strerror(errno));
        goto done;
    }

    memset(recv_buf, 0, sizeof(recv_buf));
    rd = recvfrom(client_fd, recv_buf, sizeof(recv_buf) - 1, 0, NULL, NULL);
    if (rd != (ssize_t)strlen(server_msg)) {
        printf("recvfrom(client) failed: read=%ld errno=%d (%s)\r\n",
               (long)rd, errno, strerror(errno));
        goto done;
    }
    recv_buf[rd] = '\0';
    if (strcmp(recv_buf, server_msg) != 0) {
        printf("recvfrom(client) mismatch: got=\"%s\"\r\n", recv_buf);
        goto done;
    }

    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags < 0) {
        printf("fcntl(F_GETFL) failed: errno=%d (%s)\r\n", errno, strerror(errno));
        goto done;
    }
    if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        printf("fcntl(F_SETFL O_NONBLOCK) failed: errno=%d (%s)\r\n", errno, strerror(errno));
        goto done;
    }

    errno = 0;
    rd = recvfrom(client_fd, recv_buf, 1, 0, NULL, NULL);
    if (rd != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        printf("nonblocking recvfrom expected EAGAIN: read=%ld errno=%d\r\n",
               (long)rd, errno);
        goto done;
    }

    printf("socket IPC demo ok: connect/accept/sendto/recvfrom/fcntl passed\r\n");
    rc = 0;

done:
    if (accepted_fd >= 0) close(accepted_fd);
    if (client_fd >= 0) close(client_fd);
    if (server_fd >= 0) close(server_fd);
    return rc;
}

int main(void) {
    printf("hello from userspace!\r\n");

    int rc_vma = run_vma_syscall_demo();
    printf("VMA syscall demo %s\r\n", rc_vma == 0 ? "passed" : "failed");

    int rc_fd = run_resource_fd_demo();
    printf("Resource FD demo %s\r\n", rc_fd == 0 ? "passed" : "failed");

    int rc_socket = run_socket_ipc_demo();
    printf("Socket IPC demo %s\r\n", rc_socket == 0 ? "passed" : "failed");

    return (rc_vma == 0 && rc_fd == 0 && rc_socket == 0) ? 0 : 1;
}
