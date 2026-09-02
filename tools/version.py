#!/usr/bin/env python3
"""The BTP version number -- read it, check it, or set it.

BTP is one SemVer line and the number lives in include/btp/version.hpp
(docs/library.md section 10). library.json carries a copy for PlatformIO, which
never runs CMake. This script is the one thing that touches both.

    python tools/version.py            # print MAJOR.MINOR.PATCH
    python tools/version.py --check    # exit non-zero if the copies disagree
    python tools/version.py 2.11.0     # rewrite version.hpp and library.json

After a bump: commit, then `git tag vX.Y.Z`. CMake re-checks the two copies on
every configure, and CI runs `--check`.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_HPP = ROOT / "include" / "btp" / "version.hpp"
LIBRARY_JSON = ROOT / "library.json"

PARTS = ("Major", "Minor", "Patch")


def read_hpp():
    text = VERSION_HPP.read_text(encoding="utf-8")
    out = []
    for part in PARTS:
        m = re.search(r"kLibraryVersion" + part + r"\s*=\s*(\d+)", text)
        if not m:
            sys.exit("version.py: no kLibraryVersion{} in {}".format(
                part, VERSION_HPP))
        out.append(int(m.group(1)))
    return tuple(out)


def read_json_version():
    text = LIBRARY_JSON.read_text(encoding="utf-8")
    m = re.search(r'"version"\s*:\s*"(\d+\.\d+\.\d+)"', text)
    if not m:
        sys.exit("version.py: no \"version\" string in {}".format(LIBRARY_JSON))
    return m.group(1)


def fmt(triple):
    return "{}.{}.{}".format(*triple)


def set_hpp(triple):
    text = VERSION_HPP.read_text(encoding="utf-8")
    for part, n in zip(PARTS, triple):
        text = re.sub(r"(kLibraryVersion" + part + r"\s*=\s*)\d+(U?)",
                      lambda m, n=n: m.group(1) + str(n) + m.group(2), text)
    VERSION_HPP.write_text(text, encoding="utf-8")


def set_json(triple):
    text = LIBRARY_JSON.read_text(encoding="utf-8")
    text = re.sub(r'("version"\s*:\s*")\d+\.\d+\.\d+(")',
                  lambda m: m.group(1) + fmt(triple) + m.group(2), text)
    LIBRARY_JSON.write_text(text, encoding="utf-8")


def main(argv):
    hpp = read_hpp()

    if not argv:
        print(fmt(hpp))
        return 0

    if argv[0] == "--check":
        js = read_json_version()
        if js != fmt(hpp):
            sys.stderr.write(
                "version mismatch:\n"
                "  include/btp/version.hpp  {}\n"
                "  library.json             {}\n"
                "fix with: python tools/version.py {}\n".format(
                    fmt(hpp), js, fmt(hpp)))
            return 1
        print("ok: {}".format(fmt(hpp)))
        return 0

    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", argv[0])
    if not m:
        sys.exit("version.py: not a version: {!r} (want X.Y.Z)".format(argv[0]))
    triple = tuple(int(x) for x in m.groups())
    set_hpp(triple)
    set_json(triple)
    print("set {} in include/btp/version.hpp and library.json".format(
        fmt(triple)))
    print("next: commit, then  git tag v{}".format(fmt(triple)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
