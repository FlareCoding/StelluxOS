#!/usr/bin/env python3
"""Plays the far end of the FTDI chip QEMU emulates for the kernel's serial port tests.

Usage: serial_test_host.py <serial log> <socket path>

QEMU's usb-serial device is plugged in while a client holds its socket open and unplugged when the client closes it,
and the client's bytes reach the chip as if received on its UART. The kernel's tests ask for each step with a line on
the serial log, which this script follows until the tests complete:

    STLX_SERIAL_HOST connect                 plugs the chip in
    STLX_SERIAL_HOST send <first> <count>    sends <count> bytes counting up from <first>, wrapping at 256
    STLX_SERIAL_HOST disconnect              unplugs the chip
"""

import socket
import sys
import time

REQUEST_PREFIX = "STLX_SERIAL_HOST "
TESTS_COMPLETE = "STLX_TESTS_COMPLETE"
POLL_SECONDS = 0.05

# QEMU creates the socket as it starts, so the first connect may come before it exists
CONNECT_TIMEOUT_SECONDS = 10


def follow_lines(path):
    with open(path, "rb") as log:
        pending = b""
        while True:
            chunk = log.read()
            if not chunk:
                time.sleep(POLL_SECONDS)
                continue

            pending += chunk
            *lines, pending = pending.split(b"\n")
            for line in lines:
                yield line.decode("ascii", "replace").strip()


def connect(path):
    deadline = time.monotonic() + CONNECT_TIMEOUT_SECONDS
    while True:
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.connect(path)
            return client
        except OSError:
            client.close()
            if time.monotonic() > deadline:
                raise

            time.sleep(POLL_SECONDS)


def counting_bytes(first, count):
    return bytes((first + i) % 256 for i in range(count))


def main():
    log_path, socket_path = sys.argv[1], sys.argv[2]
    chip = None

    for line in follow_lines(log_path):
        if line.startswith(TESTS_COMPLETE):
            break

        if not line.startswith(REQUEST_PREFIX):
            continue

        request = line[len(REQUEST_PREFIX):].split()
        if request[0] == "connect" and chip is None:
            chip = connect(socket_path)
        elif request[0] == "send" and chip is not None:
            chip.sendall(counting_bytes(int(request[1]), int(request[2])))
        elif request[0] == "disconnect" and chip is not None:
            chip.close()
            chip = None


if __name__ == "__main__":
    main()
