#!/usr/bin/env python3
"""Plays the far end of the FTDI chip QEMU emulates for the kernel's serial port tests.

Usage: serial_test_host.py <serial log> <socket path> <QEMU trace log>

QEMU's usb-serial device is plugged in while a client holds its socket open and unplugged when the client closes it,
and the client's bytes reach the chip as if received on its UART. The kernel's tests ask for each step with a line on
the serial log, which this script follows until the tests complete:

    STLX_SERIAL_HOST connect                 plugs the chip in
    STLX_SERIAL_HOST send <first> <count>    sends <count> bytes counting up from <first>, wrapping at 256
    STLX_SERIAL_HOST echo <count>            sends back the next <count> bytes the chip sends
    STLX_SERIAL_HOST baud <rate>             sends ACK once QEMU's trace shows the chip set within 3 percent of
                                             <rate>, or NAK when it does not within a few seconds
    STLX_SERIAL_HOST control <request> <value>
                                             sends ACK once QEMU's trace shows the chip given the vendor request
                                             <request> with <value>, both in hex, or NAK when it does not within a
                                             few seconds
    STLX_SERIAL_HOST disconnect              unplugs the chip
"""

import re
import socket
import sys
import time

REQUEST_PREFIX = "STLX_SERIAL_HOST "
TESTS_COMPLETE = "STLX_TESTS_COMPLETE"
POLL_SECONDS = 0.05

# QEMU creates the socket as it starts, so the first connect may come before it exists
CONNECT_TIMEOUT_SECONDS = 10

# How long the chip's bytes and QEMU's trace lines may take to arrive
ECHO_TIMEOUT_SECONDS = 10
TRACE_TIMEOUT_SECONDS = 5

ACK = b"\x06"
NAK = b"\x15"

# The rate is the last number of each usb_serial_set_baud line
SET_BAUD = re.compile(r"usb_serial_set_baud\b.*?(\d+)\s*$")
MAX_RATE_ERROR = 0.03

# A usb_serial_handle_control line names the request as its request type and code, then the request's value
HANDLE_CONTROL = re.compile(r"usb_serial_handle_control\b.*got control 0x([0-9a-f]+), value 0x([0-9a-f]+)")


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


def echo(chip, count):
    chip.settimeout(ECHO_TIMEOUT_SECONDS)
    received = b""
    try:
        while len(received) < count:
            chunk = chip.recv(count - len(received))
            if not chunk:
                break

            received += chunk
    except socket.timeout:
        pass

    chip.settimeout(None)
    chip.sendall(received)


# The speed the chip was last set to, as QEMU traces it
def last_traced_rate(trace_path):
    try:
        with open(trace_path, "r", errors="replace") as trace:
            rates = [int(match.group(1)) for match in map(SET_BAUD.search, trace) if match]
    except OSError:
        return None

    return rates[-1] if rates else None


def confirm_rate(chip, trace_path, wanted):
    deadline = time.monotonic() + TRACE_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        rate = last_traced_rate(trace_path)
        if rate is not None and abs(rate - wanted) <= wanted * MAX_RATE_ERROR:
            chip.sendall(ACK)
            return

        time.sleep(POLL_SECONDS)

    chip.sendall(NAK)


# The vendor requests the chip was given, as QEMU traces them
def traced_controls(trace_path):
    try:
        with open(trace_path, "r", errors="replace") as trace:
            return {(int(match.group(1), 16), int(match.group(2), 16)) for match in map(HANDLE_CONTROL.search, trace)
                    if match}
    except OSError:
        return set()


def confirm_control(chip, trace_path, request, value):
    deadline = time.monotonic() + TRACE_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if (request, value) in traced_controls(trace_path):
            chip.sendall(ACK)
            return

        time.sleep(POLL_SECONDS)

    chip.sendall(NAK)


def main():
    log_path, socket_path, trace_path = sys.argv[1], sys.argv[2], sys.argv[3]
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
        elif request[0] == "echo" and chip is not None:
            echo(chip, int(request[1]))
        elif request[0] == "baud" and chip is not None:
            confirm_rate(chip, trace_path, int(request[1]))
        elif request[0] == "control" and chip is not None:
            confirm_control(chip, trace_path, int(request[1], 16), int(request[2], 16))
        elif request[0] == "disconnect" and chip is not None:
            chip.close()
            chip = None


if __name__ == "__main__":
    main()
