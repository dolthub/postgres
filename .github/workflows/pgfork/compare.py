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
"""compare.py --suite <name> --title <title> --main-sha <sha> --pr-sha <sha> --out <comment.md> [--match <prefix>]...
[--exclude <prefix>]... [--by-suite] [--detail] [--informational <name>]... --platform <name> <main results> <pr
results>...: compares the results.py files of main and of the pull request for the tests whose names start with a
--match prefix (every test without one) and with no --exclude prefix, platform by platform, and writes the pull
request's comment, listing each regression. A test regresses when it passes on main and fails on the pull request, or
is missing from a pull request run that never reached it; the script exits 1 when a platform not named by
--informational has a regression. A results file that doesn't exist is a run that produced no results: on the pull
request every test main passes then regresses, and on main nothing can regress. A pull request run recorded as not
run (results.py --not-run) has no regressions, since its tree builds no server by design. --by-suite gives each meson
suite its own row of rates, and --detail also lists the tests that newly pass, that fail on both, and that only the
pull request has."""

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
    return {"tests": tests, "complete": data["complete"], "incomplete": data["incomplete"],
            "not_run": data.get("not_run", False)}


def reached(run, test):
    """Reports whether a run reached the point where it would have run the test, had the test existed: a pg_regress
    test's meson test ran to its end, or a meson test's whole run did."""
    if ":" in test:
        return run["complete"].get(test.split(":", 1)[0], False)
    return not run["incomplete"]


def rate(run):
    """Returns a run's pass rate as text."""
    if run is None:
        return "no results"
    if run["not_run"]:
        return "not run (no server built yet)"
    statuses = list(run["tests"].values())
    ran = len(statuses) - statuses.count("skip")
    passed = statuses.count("ok")
    text = f"{passed} of {ran} pass ({100 * passed / ran:.1f}%)" if ran else "no tests ran"
    text += f", {statuses.count('skip')} skipped" if statuses.count("skip") else ""
    stopped = run["incomplete"] or not all(reached(run, test) for test in run["tests"] if ":" in test)
    return text + (", stopped before its end" if stopped else "")


def suite_of(test):
    """Returns the meson suite a test belongs to, the first part of its name."""
    return test.split("/", 1)[0].split(":", 1)[0]


def subset(run, suite):
    """Returns the part of a run that is one suite's tests, or None for a run without results."""
    if run is None:
        return None
    return {**run, "tests": {t: status for t, status in run["tests"].items() if suite_of(t) == suite}}


def changes(main, pr):
    """Returns the tests that regressed, that newly pass, that fail on both, and that only the pull request has, none
    of them for a pull request run that was not run."""
    if pr and pr["not_run"]:
        return [], [], [], []
    main_tests = main["tests"] if main else {}
    pr_tests = pr["tests"] if pr else {}
    regressed, fixed, failing, added = [], [], [], []
    for test, status in sorted(main_tests.items()):
        mine = pr_tests.get(test)
        if mine is None:
            if status == "ok" and (pr is None or not reached(pr, test)):
                regressed.append((test, "not reached"))
        elif status == "ok" and mine == "fail":
            regressed.append((test, "fails"))
        elif status == "fail" and mine == "ok":
            fixed.append(test)
        elif status == "fail" and mine == "fail":
            failing.append(test)
    for test, status in sorted(pr_tests.items()):
        if test not in main_tests:
            added.append((test, status))
    return regressed, fixed, failing, added


def listing(title, tests, prefixes):
    """Returns a collapsed list of tests, each a name and an optional note, with the prefix they matched taken off their
    names, or nothing when there are none."""
    if not tests:
        return []
    lines = []
    for test, note in tests:
        name = next((test[len(p):] for p in prefixes if test.startswith(p)), test)
        lines.append(f"- `{name}`" + (f" ({note})" if note else ""))
    return [f"<details><summary>{title} ({len(lines)})</summary>", "", *lines, "", "</details>", ""]


def main():
    """Writes the comment and exits 1 on a regression."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", required=True)
    parser.add_argument("--title", required=True)
    parser.add_argument("--main-sha", required=True)
    parser.add_argument("--pr-sha", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--match", action="append", default=[])
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--detail", action="store_true")
    parser.add_argument("--by-suite", action="store_true")
    parser.add_argument("--platform", nargs=3, action="append", required=True, metavar=("NAME", "MAIN", "PR"))
    parser.add_argument("--informational", action="append", default=[])
    args = parser.parse_args()

    rows, sections, failed, not_run = [], [], [], []
    for name, main_path, pr_path in args.platform:
        main, pr = load(main_path, args.match, args.exclude), load(pr_path, args.match, args.exclude)
        regressed, fixed, failing, added = changes(main, pr)
        informational = name in args.informational
        if regressed and not informational:
            failed.append(name)
        if pr and pr["not_run"]:
            not_run.append(name)
        label = f"{name} (informational)" if informational else name
        if args.by_suite:
            for suite in sorted({suite_of(t) for run in (main, pr) if run for t in run["tests"]}):
                part = changes(subset(main, suite), subset(pr, suite))
                rows.append(f"| {label} | {suite} | {rate(subset(main, suite))} | {rate(subset(pr, suite))} | "
                            f"{len(part[0])} | {len(part[1])} |")
        else:
            rows.append(f"| {label} | {rate(main)} | {rate(pr)} | {len(regressed)} | {len(fixed)} |")
        section = listing("Regressions: pass on main, fail on the PR", regressed, args.match)
        if args.detail:
            section += listing("Newly passing", [(t, "") for t in fixed], args.match)
            section += listing("Failing on both", [(t, "") for t in failing], args.match)
            section += listing("Only on the PR", added, args.match)
        if section:
            sections += [f"### {label}", "", *section]

    if failed:
        verdict = f"**Regressions on {', '.join(failed)}:** a test that passes on main fails on this PR."
    elif not_run:
        verdict = (f"**Not run on {', '.join(not_run)}:** this PR's tree builds no server yet (pgfork/targets.sh), so "
                   "these tests have nothing to compare.")
    else:
        verdict = "**No regressions:** every test that passes on main passes on this PR."
    lines = [f"<!-- pgfork-ci:{args.suite} -->", f"## {args.title}", "",
             f"main `{args.main_sha[:11]}` against this PR `{args.pr_sha[:11]}` merged with main.", "",
             "| Platform | Suite | main | PR | Regressions | Newly passing |" if args.by_suite
             else "| Platform | main | PR | Regressions | Newly passing |",
             "|---|---|---|---|---|---|" if args.by_suite else "|---|---|---|---|---|", *rows, "", verdict, "",
             *sections]
    text = "\n".join(lines).rstrip() + "\n"
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    print(text)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
