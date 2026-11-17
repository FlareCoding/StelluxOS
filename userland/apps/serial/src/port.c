#define _DEFAULT_SOURCE

#include "port.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MS_PER_SECOND 1000
#define NS_PER_MS     1000000L

static void sleep_for_ms(uint32_t ms) {
    struct timespec pause = { .tv_sec = ms / MS_PER_SECOND, .tv_nsec = (long)(ms % MS_PER_SECOND) * NS_PER_MS };
    nanosleep(&pause, NULL);
}

int port_open(serial_port* port) {
    char path[sizeof(PORT_DIRECTORY) + NAME_MAX + 1];
    snprintf(path, sizeof(path), "%s/%s", PORT_DIRECTORY, port->name);

    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    port->fd = fd;
    return 0;
}

int port_set_line(serial_port* port, const line_settings* settings) {
    struct stlx_termios2 termios;
    if (ioctl(port->fd, STLX_TCGETS2, &termios) != 0) {
        return -1;
    }

    line_settings_apply(settings, &termios);
    return ioctl(port->fd, STLX_TCSETSF2, &termios);
}

int port_set_levels(serial_port* port, const line_request* request) {
    if (request->dtr == LEVEL_KEEP && request->rts == LEVEL_KEEP) {
        return 0;
    }

    if (request->dtr != LEVEL_KEEP && request->rts != LEVEL_KEEP) {
        int lines = (request->dtr == LEVEL_RAISED ? TIOCM_DTR : 0) | (request->rts == LEVEL_RAISED ? TIOCM_RTS : 0);
        return ioctl(port->fd, TIOCMSET, &lines);
    }

    int line = request->dtr != LEVEL_KEEP ? TIOCM_DTR : TIOCM_RTS;
    line_level level = request->dtr != LEVEL_KEEP ? request->dtr : request->rts;
    return ioctl(port->fd, level == LEVEL_RAISED ? TIOCMBIS : TIOCMBIC, &line);
}

int port_pulse(serial_port* port, line_pulse pulse) {
    int line = pulse == PULSE_DTR ? TIOCM_DTR : TIOCM_RTS;
    int lines = 0;
    if (ioctl(port->fd, TIOCMGET, &lines) != 0) {
        return -1;
    }

    bool is_raised = (lines & line) != 0;
    if (ioctl(port->fd, is_raised ? TIOCMBIC : TIOCMBIS, &line) != 0) {
        return -1;
    }

    sleep_for_ms(PULSE_MS);
    return ioctl(port->fd, is_raised ? TIOCMBIS : TIOCMBIC, &line);
}

int port_break(serial_port* port, uint32_t ms) {
    if (ioctl(port->fd, TIOCSBRK, 0) != 0) {
        return -1;
    }

    sleep_for_ms(ms);
    return ioctl(port->fd, TIOCCBRK, 0);
}

int port_count_unread(const serial_port* port, size_t* unread) {
    int count = 0;
    if (ioctl(port->fd, FIONREAD, &count) != 0) {
        return -1;
    }

    *unread = (size_t)count;
    return 0;
}

void port_close(serial_port* port) {
    if (port->fd >= 0) {
        close(port->fd);
        port->fd = -1;
    }
}
