#!/usr/bin/env python3
"""Prints the third-party code bundled in the Qt sources under a directory, from
the qt_attribution.json file Qt keeps beside each bundled component. Tests and
examples are skipped, since nothing of theirs is in the kit.

Usage: attributions.py <source dir>
"""
import json
import pathlib
import sys

FIELDS = ("Version", "LicenseId", "Copyright", "Homepage")


def components(root):
    for path in sorted(pathlib.Path(root).rglob("qt_attribution.json")):
        if {"tests", "examples"} & set(path.parts):
            continue

        # Some of Qt's files hold raw tabs inside strings
        data = json.loads(path.read_text(), strict=False)
        yield from data if isinstance(data, list) else [data]


def main():
    print("Third-party code bundled in the Qt sources this kit was built from, per qt_attribution.json.")
    print("The license texts are in the module directories beside this file, named by their SPDX identifiers.")
    for component in components(sys.argv[1]):
        print(f"\n{component.get('Name', 'Unnamed component')}")
        for field in FIELDS:
            value = component.get(field)
            if isinstance(value, list):
                value = "\n    ".join(value)

            if value:
                print(f"  {field}: {value.strip()}")


if __name__ == "__main__":
    main()
