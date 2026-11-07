#!/usr/bin/env python3
"""Writes a case list for each named dEQP-VK test group, as <group>.txt in an output directory.

The release's mustpass lists the cases a conformant driver must run without a failure. A
group's list holds at most the given number of its mustpass cases, the same on every build of
one release, and a group with no case in the mustpass stops the script with an error.

The group's cases, in mustpass order, are cut into stretches of the shortest length that makes
at most that many, the last taking what is left, and from each stretch the list takes the case
whose name has the lowest SHA-256. One case per stretch spreads the list over the whole group,
and the hash picks it because a test's variants repeat in a fixed order, so a fixed stride
would pick some variants far more often than others.

Usage: caselists.py <mustpass dir> <output dir> <most cases per list> <group>...
"""
import hashlib
import math
import pathlib
import sys

MUSTPASS_INDEX = "vk-default.txt"
CASE_PREFIX = "dEQP-VK."


def read_group_cases(mustpass_dir, groups):
    """The mustpass cases of each group in mustpass order, keyed by group name."""
    cases = {group: [] for group in groups}
    for list_name in (mustpass_dir / MUSTPASS_INDEX).read_text().split():
        for case in (mustpass_dir / list_name).read_text().split():
            group = case.removeprefix(CASE_PREFIX).split(".", 1)[0]
            if group in cases:
                cases[group].append(case)

    return cases


def name_hash(case):
    return hashlib.sha256(case.encode()).digest()


def sample_cases(cases, max_cases):
    stretch_length = math.ceil(len(cases) / max_cases)
    stretches = (cases[start:start + stretch_length] for start in range(0, len(cases), stretch_length))
    return [min(stretch, key=name_hash) for stretch in stretches]


def main():
    mustpass_dir = pathlib.Path(sys.argv[1])
    output_dir = pathlib.Path(sys.argv[2])
    max_cases = int(sys.argv[3])
    groups = sys.argv[4:]

    output_dir.mkdir(parents=True, exist_ok=True)
    for group, cases in read_group_cases(mustpass_dir, groups).items():
        if not cases:
            sys.exit(f"vk-gl-cts: the mustpass holds no case of {CASE_PREFIX}{group}, rename or drop the group")

        sample = sample_cases(cases, max_cases)
        (output_dir / f"{group}.txt").write_text("".join(case + "\n" for case in sample))


if __name__ == "__main__":
    main()
