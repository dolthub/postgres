#!/usr/bin/env python3
#
# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
"""test_include_tree.py: runs include_tree.py over synthetic include folders, an identical pair and one per way a
fork's headers can differ, and fails unless each is judged as expected. Runs whenever the gates change."""

import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import include_tree  # noqa: E402

STOCK = {"a.h": "#define A 1\nstruct s { int x; int y; };\n", "sub/b.h": "#define B 2\n\n\n"}
CASES = {
    "identical": (dict(STOCK), [], None),
    "a listed addition": ({**STOCK, "a.h": STOCK["a.h"] + "#define A_FORK 3\n"},
                          [("a.h", "struct s { int x; int y; };", "#define A_FORK 3")], None),
    "a run of listed additions": ({**STOCK, "a.h": STOCK["a.h"] + "#define A_FORK 3\n#define A_FORK2 4\n"},
                                  [("a.h", "struct s { int x; int y; };", "#define A_FORK 3"),
                                   ("a.h", "#define A_FORK 3", "#define A_FORK2 4")], None),
    "a listed line moved": ({**STOCK, "a.h": "#define A 1\n#define A_FORK 3\nstruct s { int x; int y; };\n"},
                            [("a.h", "struct s { int x; int y; };", "#define A_FORK 3")], "differs from stock"),
    "an anchor that recurs": ({**STOCK, "sub/b.h": "#define B 2\n\n#define B_FORK 4\n\n"}, [("sub/b.h", "", "#define B_FORK 4")],
                              "rather than once"),
    "a run with a blank line": ({**STOCK, "sub/b.h": "#define B 2\n\n#define B_FORK 4\n\n\n"},
                                [("sub/b.h", "#define B 2", ""), ("sub/b.h", "", "#define B_FORK 4")],
                                "which the stock header already holds"),
    "a first-line addition": ({**STOCK, "sub/b.h": "#define B_FIRST 1\n" + STOCK["sub/b.h"]},
                              [("sub/b.h", "START", "#define B_FIRST 1")], None),
    "a line ending changed": ({**STOCK, "sub/b.h": "#define B 2\r\n"}, [], "differs from stock"),
    "an unlisted addition": ({**STOCK, "a.h": STOCK["a.h"] + "#define A_FORK 3\n"}, [], "differs from stock"),
    "a listed line used twice": ({**STOCK, "a.h": STOCK["a.h"] + "#define A_FORK 3\n#define A_FORK 3\n"},
                                 [("a.h", "struct s { int x; int y; };", "#define A_FORK 3")], "differs from stock"),
    "a reordered struct": ({**STOCK, "a.h": "#define A 1\nstruct s { int y; int x; };\n"}, [], "differs from stock"),
    "a changed line": ({**STOCK, "sub/b.h": "#define B 3\n"}, [], "differs from stock"),
    "a missing header": ({"a.h": STOCK["a.h"]}, [], "sub/b.h isn't installed"),
    "an added header": ({**STOCK, "c.h": "#define C 1\n"}, [], "c.h is installed but stock Postgres has no such header"),
}


def build(root, files):
    """Writes a synthetic include folder."""
    for name, text in files.items():
        path = os.path.join(root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


def main():
    """Checks every case, printing each mismatch."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    failures = 0
    for name, (fork, listed, expected) in CASES.items():
        with tempfile.TemporaryDirectory() as work:
            build(os.path.join(work, "fork"), fork)
            build(os.path.join(work, "stock"), STOCK)
            include_tree.allowed_lines = lambda: {h: __import__("collections").Counter(
                (after, line) for header, after, line in listed if header == h) for h, _, _ in listed}
            found = include_tree.problems(os.path.join(work, "fork"), os.path.join(work, "stock"))
        if (expected is None and found) or (expected is not None and not any(expected in p for p in found)):
            failures += 1
            print(f"{name}: got {found}, expected {expected or 'no problems'}")
    print("include-tree cases pass" if not failures else f"{failures} include-tree cases fail")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
