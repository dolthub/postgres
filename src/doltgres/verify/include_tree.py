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
"""include_tree.py <fork include folder> <stock include folder>: compares the headers the fork's build installs with a
stock build's of the same options, so that an extension built for stock Postgres sees no difference (the owner's
decision of 2026-11-07). Every stock header must be installed byte-identical, except that include-differences.tsv may
list lines a header adds, each right after a given line (a line the stock header holds exactly once, the addition
before it in a run, or START for the header's first line) and none a line the stock header holds; with those
lines left out, the header must equal the stock one byte for byte, line endings included, so a reordered, changed, or
moved line is a problem, as are a missing header and an added one. Only the coordinator's
refs change the table, each line with PRevPF's YES."""

import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
START = "START"


def headers(root):
    """Returns every file under an include folder, by its path relative to it."""
    found = {}
    for folder, _, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            found[os.path.relpath(path, root)] = path
    return found


def allowed_lines():
    """Returns include-differences.tsv's additions, as {header: Counter of (the line it follows, the added line)}."""
    allowed = collections.defaultdict(collections.Counter)
    with open(os.path.join(HERE, "include-differences.tsv"), encoding="utf-8") as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                header, after, added = line.rstrip("\n").split("\t")[:3]
                allowed[header][(after, added)] += 1
    return allowed


def problems(fork_root, stock_root):
    """Returns how the fork's installed headers differ from the stock ones beyond the listed additions."""
    fork, stock, allowed = headers(fork_root), headers(stock_root), allowed_lines()
    found = [f"{name} isn't installed" for name in sorted(set(stock) - set(fork))]
    found += [f"{name} is installed but stock Postgres has no such header" for name in sorted(set(fork) - set(stock))]
    for name in sorted(set(fork) & set(stock)):
        with open(fork[name], "rb") as f:
            ours = f.read()
        with open(stock[name], "rb") as f:
            theirs = f.read()
        if ours == theirs:
            continue
        stock_lines = collections.Counter(line.rstrip("\r\n") for line in theirs.decode(errors="replace").splitlines())
        additions = {added for _, added in allowed.get(name, {})}
        for after, added in allowed.get(name, {}):
            if stock_lines[added]:
                found.append(f"include-differences.tsv adds {added!r} to {name}, which the stock header already holds")
            if after != START and after not in additions and stock_lines[after] != 1:
                found.append(f"include-differences.tsv anchors {added!r} in {name} after {after!r}, which the stock header "
                             f"holds {stock_lines[after]} times rather than once")
        budget = collections.Counter(allowed.get(name, collections.Counter()))
        kept, previous = [], START
        for line in ours.decode(errors="replace").splitlines(keepends=True):
            text = line.rstrip("\r\n")
            if budget[(previous, text)] > 0:
                budget[(previous, text)] -= 1
            else:
                kept.append(line)
            previous = text
        if kept != theirs.decode(errors="replace").splitlines(keepends=True):
            found.append(f"{name} differs from stock beyond the lines include-differences.tsv lets it add")
    return found


def main():
    """Compares the two include folders given and prints what differs."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    found = problems(*(os.path.abspath(a) for a in sys.argv[1:3]))
    for problem in found:
        print(problem)
    print("the installed headers match" if not found else f"the installed headers differ: {len(found)} problems")
    sys.exit(1 if found else 0)


if __name__ == "__main__":
    main()
