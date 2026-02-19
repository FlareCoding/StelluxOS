#!/usr/bin/env python3
"""
Run Stellux in-kernel unit tests in QEMU and parse serial output.
"""

from __future__ import annotations

import argparse
import os
import select
import signal
import subprocess
import sys
import time
from typing import Optional


PASS_MARKER = "[TEST_RESULT] PASS"
FAIL_MARKER = "[TEST_RESULT] FAIL"


def terminate_process_group(proc: subprocess.Popen[str], grace_seconds: float = 3.0) -> None:
    if proc.poll() is not None:
        return

    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return

    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return
        time.sleep(0.05)

    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def sanitize_tag(value: str, fallback: str = "all") -> str:
    cleaned = "".join(ch if ch.isalnum() else "_" for ch in value)
    cleaned = cleaned.strip("_")
    if not cleaned:
        cleaned = fallback
    return cleaned[:48]


def build_command(args: argparse.Namespace) -> list[str]:
    filter_tag = sanitize_tag(args.filter, fallback="all")
    seed_tag = sanitize_tag(str(args.seed), fallback="seed")
    build_dir = (
        f"build/unit-tests-{args.arch}-"
        f"{filter_tag}-ff{args.fail_fast}-r{args.repeat}-s{seed_tag}"
    )
    image_dir = (
        f"images/unit-tests-{args.arch}-"
        f"{filter_tag}-ff{args.fail_fast}-r{args.repeat}-s{seed_tag}"
    )

    return [
        "make",
        "run-headless",
        f"ARCH={args.arch}",
        f"BUILD_DIR={build_dir}",
        f"IMAGE_DIR={image_dir}",
        "UNIT_TEST=1",
        f"UNIT_TEST_FILTER={args.filter}",
        f"UNIT_TEST_FAIL_FAST={args.fail_fast}",
        f"UNIT_TEST_REPEAT={args.repeat}",
        f"UNIT_TEST_SEED={args.seed}",
    ]


def run(args: argparse.Namespace) -> int:
    cmd = build_command(args)
    print(f"[HOST_TEST] command: {' '.join(cmd)}", flush=True)
    print(f"[HOST_TEST] timeout: {args.timeout}s", flush=True)

    proc = subprocess.Popen(
        cmd,
        cwd=args.workspace,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=False,
        bufsize=0,
        preexec_fn=os.setsid,
    )

    assert proc.stdout is not None
    stdout_fd = proc.stdout.fileno()

    start = time.monotonic()
    result: Optional[bool] = None
    collected: list[str] = []
    tail = ""

    while True:
        if time.monotonic() - start > args.timeout:
            print("[HOST_TEST] timeout exceeded", flush=True)
            terminate_process_group(proc)
            result = False
            break

        ready, _, _ = select.select([stdout_fd], [], [], 0.2)
        if ready:
            chunk = os.read(stdout_fd, 4096)
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                collected.append(text)
                print(text, end="", flush=True)

                tail = (tail + text)[-8192:]
                if PASS_MARKER in tail:
                    print("[HOST_TEST] PASS marker detected", flush=True)
                    terminate_process_group(proc)
                    result = True
                    break
                if FAIL_MARKER in tail:
                    print("[HOST_TEST] FAIL marker detected", flush=True)
                    terminate_process_group(proc)
                    result = False
                    break
                continue

        if proc.poll() is not None:
            break

    if result is None:
        rc = proc.wait(timeout=5)
        if rc != 0:
            print(f"[HOST_TEST] process exited with rc={rc}", flush=True)
            result = False
        else:
            print("[HOST_TEST] process exited without PASS/FAIL marker", flush=True)
            result = False
    else:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            terminate_process_group(proc)

    if args.log_file:
        os.makedirs(os.path.dirname(args.log_file), exist_ok=True)
        with open(args.log_file, "w", encoding="utf-8") as f:
            f.writelines(collected)
        print(f"[HOST_TEST] wrote log: {args.log_file}", flush=True)

    if result:
        print("[HOST_TEST] unit tests passed", flush=True)
        return 0

    print("[HOST_TEST] unit tests failed", flush=True)
    return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Stellux in-kernel unit tests")
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    parser.add_argument("--filter", default="", help="Suite prefix or suite.case filter")
    parser.add_argument("--fail-fast", type=int, default=0, choices=(0, 1))
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--seed", default="0xC0FFEE")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--workspace", default=".")
    parser.add_argument("--log-file", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.repeat < 1:
        print("[HOST_TEST] repeat must be >= 1", flush=True)
        return 2
    if args.timeout < 1:
        print("[HOST_TEST] timeout must be >= 1", flush=True)
        return 2
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
