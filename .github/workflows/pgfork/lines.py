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
"""lines.py <tree> <base> <head> <comment.md>: writes the pull request's comment counting the lines its diff from base
to head adds and removes in three categories, Postgres (Postgres' own files the fork changes), Doltgres (the fork's
code the server runs), and testing (tests, code that exists only for tests, and the simulator), and in the excluded
rest (CI, build files, documents, vendored third-party code, tables, and binaries), so the four add up to the diff's
total. A line of the fork's C inside a `#ifdef DOLTGRES_TEST_...` block counts as testing."""

import re
import subprocess
import sys
from collections import defaultdict

CATEGORIES = ["Postgres", "Doltgres", "Testing", "Excluded"]
FORK = ("src/doltgres/", "src/include/doltgres/")
TESTING_DIRS = ("src/doltgres/test/", "src/doltgres/sim/", "src/doltgres/simc/", "src/doltgres/verify/", "src/test/")
TEST_PART = re.compile(r"(^|/)(tests?|testdata|fixtures)(/|$)|(^|[/_])test[s]?[_.]|_tests?\.[a-z]+$")
TEST_OPEN = re.compile(r"^\s*#\s*(ifdef\s+DOLTGRES_TEST_\w+|if\s+.*\bDOLTGRES_TEST_\w+)")
COND_OPEN = re.compile(r"^\s*#\s*if")
COND_ELSE = re.compile(r"^\s*#\s*(else|elif)\b")
COND_END = re.compile(r"^\s*#\s*endif\b")


def git(tree, *args):
    """Runs git in the tree and returns its output."""
    return subprocess.run(["git", "-C", tree, *args], check=True, capture_output=True).stdout


def excluded(path):
    """Says whether a path is outside the three categories: CI, build files, documents, vendored code, and tables."""
    name = path.rsplit("/", 1)[-1]
    return (path.startswith((".github/", "doc/", "src/doltgres/third_party/", "src/makefiles/", "src/tools/"))
            or name in ("meson.build", "meson_options.txt", "Makefile", "Makefile.global.in", ".gitattributes",
                        ".gitignore", "VERSION")
            or name.endswith((".md", ".tsv", ".list", ".pl", ".toml", ".lock", ".json", ".txt")))


def category(path):
    """Returns a path's category, before a C file's test blocks are taken out."""
    if excluded(path):
        return "Excluded"
    if path.startswith(TESTING_DIRS) or (path.startswith(FORK) and TEST_PART.search(path)):
        return "Testing"
    if path.startswith(FORK):
        return "Doltgres"
    return "Testing" if TEST_PART.search(path) and path.startswith("src/test") else "Postgres"


def test_lines(text):
    """Returns the 1-based numbers of a C file's lines inside `#ifdef DOLTGRES_TEST_...` blocks, the directives
    included."""
    lines, stack, inside = set(), [], False
    for number, line in enumerate(text.splitlines(), 1):
        if TEST_OPEN.match(line):
            stack.append((inside, True))
            inside = True
            lines.add(number)
        elif COND_OPEN.match(line):
            stack.append((inside, False))
            if inside:
                lines.add(number)
        elif COND_ELSE.match(line) and stack:
            outer, opened = stack[-1]
            if inside or opened:
                lines.add(number)
            if opened:
                inside = outer
        elif COND_END.match(line) and stack:
            outer, opened = stack.pop()
            if inside or opened:
                lines.add(number)
            inside = outer
        elif inside:
            lines.add(number)
    return lines


def main():
    """Counts the diff's lines by category and writes the comment."""
    tree, base, head, out = sys.argv[1:5]
    added, removed = defaultdict(int), defaultdict(int)
    for row in git(tree, "diff", "--numstat", "--no-renames", "-z", base, head).decode().split("\0"):
        if not row:
            continue
        plus, minus, path = row.split("\t", 2)
        if plus == "-":
            continue
        kind = category(path)
        if kind != "Doltgres" or not path.endswith((".c", ".h")):
            added[kind] += int(plus)
            removed[kind] += int(minus)
            continue
        sides = {}
        for side, rev in (("-", base), ("+", head)):
            try:
                sides[side] = test_lines(git(tree, "show", f"{rev}:{path}").decode(errors="replace"))
            except subprocess.CalledProcessError:
                sides[side] = set()
        diff = git(tree, "diff", "-U0", "--no-renames", base, head, "--", path).decode(errors="replace")
        old = new = 0
        for line in diff.splitlines():
            hunk = re.match(r"^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@", line)
            if hunk:
                old, new = int(hunk.group(1)), int(hunk.group(2))
            elif line.startswith("+") and not line.startswith("+++"):
                added["Testing" if new in sides["+"] else "Doltgres"] += 1
                new += 1
            elif line.startswith("-") and not line.startswith("---"):
                removed["Testing" if old in sides["-"] else "Doltgres"] += 1
                old += 1
    total = (sum(added.values()), sum(removed.values()))
    rows = [f"| {kind} | +{added[kind]:,} | -{removed[kind]:,} |" for kind in CATEGORIES]
    text = "\n".join([
        "<!-- pgfork-ci:lines -->", "## Lines by category", "",
        f"`{base[:11]}` to `{head[:11]}`. Postgres is Postgres' own files the fork changes, Doltgres the fork's code "
        "the server runs, and Testing its tests, test-only code, and the simulator; Excluded is CI, build files, "
        "documents, vendored third-party code, and tables.", "",
        "| Category | Added | Removed |", "|---|---|---|", *rows,
        f"| **Total** | **+{total[0]:,}** | **-{total[1]:,}** |", ""])
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print(text)


if __name__ == "__main__":
    main()
