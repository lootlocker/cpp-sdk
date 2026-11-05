#!/usr/bin/env python3
"""Generate the C++ timezone table from data/windows-to-iana.tsv.

The table is data, not code (D26). Keeping the source as a plain TSV and generating the
C++ means a mapping change is a data diff rather than a code review, and it keeps the
generated file out of the way of anyone reading the converter.

Run from the repo root:

    python tools/generate_timezone_table.py

Writes src/core/timezone_table.inc. The generated file is committed, so a build does not
need Python; CI checks that regenerating produces no diff.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "data" / "windows-to-iana.tsv"
TARGET = ROOT / "src" / "core" / "timezone_table.inc"


def parse(path):
    pairs = []
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 2:
            sys.exit(f"{path}:{lineno}: expected 2 tab-separated fields, got {len(parts)}")
        windows, iana = parts[0].strip(), parts[1].strip()
        if not windows or not iana:
            sys.exit(f"{path}:{lineno}: empty field")
        pairs.append((windows, iana))
    return pairs


def main():
    pairs = parse(SOURCE)
    if not pairs:
        sys.exit(f"{SOURCE}: no entries")

    seen = set()
    for windows, _ in pairs:
        if windows in seen:
            sys.exit(f"{SOURCE}: duplicate Windows name {windows!r}")
        seen.add(windows)

    lines = [
        "// GENERATED FILE - do not edit.",
        "//",
        "// Produced by tools/generate_timezone_table.py from data/windows-to-iana.tsv.",
        "// Regenerate rather than editing this file; CI fails if it is out of date.",
        "//",
        f"// {len(pairs)} entries.",
        "",
        "// clang-format off",
    ]
    for windows, iana in pairs:
        lines.append(f'{{"{windows}", "{iana}"}},')
    lines.append("// clang-format on")
    lines.append("")

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {TARGET.relative_to(ROOT)} with {len(pairs)} entries")


if __name__ == "__main__":
    main()
