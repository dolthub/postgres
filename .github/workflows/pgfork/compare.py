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
"""compare.py --title <title> --out <section.md> [--match <prefix>]... [--exclude <prefix>]... --platform <name> <main
results> <pr results>...: compares the results.py files of main and of the pull request for the tests whose names
start with a --match prefix (every test without one) and with no --exclude prefix, platform by platform, and writes the
comment's section for them: a row per platform of each side's pass rate and the counts of regressions and fixes, then
collapsed lists of both. A test regresses when it passes on main and fails on the pull request, or is missing from a
pull request run that never reached it, and is fixed when it fails on main and passes on the pull request; the script
exits 1 when any platform has a regression. A results file that doesn't exist is a run that produced no results: on
the pull request every test main passes then regresses, and on main nothing can regress."""

import argparse
import json
import os
import sys


def load(path, prefixes, excluded):
    """Returns a results file's tests that match the prefixes and none of the excluded ones, with its completeness, or
    None when it doesn't exist."""
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    tests = {k: v for k, v in data["tests"].items()
             if (not prefixes or k.startswith(tuple(prefixes))) and not k.startswith(tuple(excluded))}
    return {"tests": tests, "complete": data["complete"], "incomplete": data["incomplete"]}


def reached(run, test):
    """Reports whether a run reached the point where it would have run the test, had the test existed: a pg_regress
    test's meson test ran to its end, or a meson test's whole run did."""
    if ":" in test:
        return run["complete"].get(test.split(":", 1)[0], False)
    return not run["incomplete"]


def rate(run):
    """Returns a run's pass rate as `X of Y (P%)`, skipped tests left out, or `no results` for a run with none."""
    statuses = list(run["tests"].values()) if run else []
    ran = len(statuses) - statuses.count("skip")
    if not ran:
        return "no results"
    passed = statuses.count("ok")
    return f"{passed} of {ran} ({100 * passed / ran:.1f}%)"


def changes(main, pr):
    """Returns the tests that regressed and the tests that the pull request fixed."""
    main_tests = main["tests"] if main else {}
    pr_tests = pr["tests"] if pr else {}
    regressed, fixed = [], []
    for test, status in sorted(main_tests.items()):
        mine = pr_tests.get(test)
        if status == "ok" and (mine == "fail" or (mine is None and (pr is None or not reached(pr, test)))):
            regressed.append(test)
        elif status == "fail" and mine == "ok":
            fixed.append(test)
    return regressed, fixed


def colored(count, color):
    """Returns a count, in the color GitHub's math markup gives it when it isn't zero."""
    return f"${{\\color{{{color}}}{count}}}$" if count else "0"


def listing(title, platform, tests, prefixes):
    """Returns a collapsed list of a platform's tests, with the prefix they matched taken off their names, or nothing
    when there are none."""
    if not tests:
        return []
    names = [next((t[len(p):] for p in prefixes if t.startswith(p)), t) for t in tests]
    return [f"<details><summary>{title} on {platform} ({len(names)})</summary>", "",
            *[f"- `{name}`" for name in names], "", "</details>", ""]


def main():
    """Writes the section and exits 1 on a regression."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--title", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--match", action="append", default=[])
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--platform", nargs=3, action="append", required=True, metavar=("NAME", "MAIN", "PR"))
    args = parser.parse_args()

    rows, lists, failed = [], [], False
    for name, main_path, pr_path in args.platform:
        main, pr = load(main_path, args.match, args.exclude), load(pr_path, args.match, args.exclude)
        regressed, fixed = changes(main, pr)
        failed = failed or bool(regressed)
        rows.append(f"| {name} | {rate(main)} | {rate(pr)} | {colored(len(regressed), 'red')} | "
                    f"{colored(len(fixed), 'lightgreen')} |")
        lists += listing("Regressions", name, regressed, args.match) + listing("Fixes", name, fixed, args.match)
    text = "\n".join([f"## {args.title}", "", "| Platform | main | PR | Regressions | Fixes |",
                      "|---|---|---|---|---|", *rows, "", *lists]).rstrip() + "\n"
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    print(text)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
