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
"""results.py [--checks | --bats | --cargo] <log> <results.json> [--incomplete], or results.py --not-run <results.json>:
reads the log `meson test` writes, or with --checks the lines of checks.sh, with --bats the TAP output of
`bats --tap`, or with --cargo the output of `cargo test`, and records each test's result as ok, fail, or skip. A
meson test that runs pg_regress is recorded once per pg_regress test, under `<meson test>:<pg_regress test>`, every
other meson test once under its meson name (`<suite>/<test>`), a check under `checks:<check>`, a bats test under
`bats:<test>`, and a cargo test under `<test binary>:<test>`. A pg_regress test whose every difference from one of its
expected files is rows reordered under a statement with no ORDER BY passes, since Postgres leaves that order undefined
(its regression chapter's "Row Ordering Differences"). Each meson test, bats run, and test binary is marked
complete when it ran to its end (a failed meson test that printed no pg_regress results never did), so a comparison
can tell a test the branch removed from one a crash or a timeout never reached; --incomplete marks the whole run as
stopped before its end. --not-run records a run whose tree, by pgfork/targets.sh, builds no server, so its suites
were never meant to run. The setup suite's steps are left out."""

import glob
import json
import os
import re
import sys

# A pg_regress status line: `ok 12   + boolean   34 ms` or `not ok 12 - boolean   34 ms`, as pg_regress.c prints it.
STATUS = re.compile(r"^\s*(not )?ok\s+\d+\s+[-+]\s+(\S+)\s+\d+ ms\s*$")
# The TAP plan pg_regress prints after its last test.
PLAN = re.compile(r"^\s*1\.\.\d+\s*$")
# The line `cargo test` prints before running a test binary, and the binary's name in it, without its hash.
RUNNING = re.compile(r"^\s*Running .*\(.*?([^/\\]+?)(?:-[0-9a-f]{16})?(?:\.exe)?\)\s*$")
# A cargo test's result line: `test name ... ok`, `... FAILED`, or `... ignored`, with an optional reason.
CARGO_STATUS = re.compile(r"^test (\S+) \.\.\. (ok|FAILED|ignored)\b")
# The result each cargo status stands for.
CARGO_RESULTS = {"ok": "ok", "FAILED": "fail", "ignored": "skip"}
# The summary `cargo test` prints after a test binary's last test.
CARGO_SUMMARY = re.compile(r"^test result: ")
# A bats TAP result line, `ok 3 name` or `not ok 3 name`, with an optional directive after `#` that isn't part of the
# test's name: `# skip reason` for a skipped test, or bats' `# timeout after 120s` for a failed one.
BATS_STATUS = re.compile(r"^(not )?ok \d+ (.*?)(?:\s+# (.*))?$")
# Meson's results for a test that ran: everything else (FAIL, TIMEOUT, ERROR, UNEXPECTEDPASS) is a failure.
RESULTS = {"OK": "ok", "EXPECTEDFAIL": "ok", "SKIP": "skip"}


def cargo(log):
    """Returns the tests and the completed test binaries of a `cargo test` output, a test that ran twice keeping its
    later result."""
    tests, complete, binary = {}, {}, None
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            running, status = RUNNING.match(line), CARGO_STATUS.match(line)
            if running:
                binary = running.group(1)
                complete[binary] = False
            elif binary and status:
                tests[f"{binary}:{status.group(1)}"] = CARGO_RESULTS[status.group(2)]
            elif binary and CARGO_SUMMARY.match(line):
                complete[binary] = True
    return tests, complete


def bats(log):
    """Returns the tests of a `bats --tap` output, and the run as complete when it printed every test its plan names."""
    tests, planned = {}, None
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            status = BATS_STATUS.match(line.rstrip("\n"))
            if planned is None and PLAN.match(line):
                planned = int(line.strip()[3:])
            elif status:
                skipped = (status.group(3) or "").lower().startswith("skip")
                tests[f"bats:{status.group(2)}"] = "skip" if skipped else "fail" if status.group(1) else "ok"
    return tests, {"bats": planned is not None and len(tests) >= planned}


def checks(log):
    """Returns the checks of a checks.sh output, which ran to its end when it wrote it."""
    with open(log, encoding="utf-8") as f:
        lines = [line.split() for line in f if line.strip()]
    return {f"checks:{name}": status for name, status in lines}, {"checks": True}


# The header pg_regress writes above each test's diff in regression.diffs, naming the expected and the results file.
DIFF_HEADER = re.compile(r"^diff (?:--strip-trailing-cr )?-U3 (\S+) (\S+)$")
# The line under a psql table's column names, and the row count that closes the table.
SEPARATOR = re.compile(r"^-+(\+-+)*$")
ROWS = re.compile(r"^\(\d+ rows?\)$")


def unordered(lines):
    """Returns a psql output's lines with each table's rows sorted, unless the statement above the table orders them
    (an ORDER BY) or the table is a plan, whose rows' order is its meaning; a row continued over lines (`+`) moves
    whole."""
    out, statement, i = [], [], 0
    while i < len(lines):
        line = lines[i]
        text = " ".join(statement).lower()
        if (SEPARATOR.match(line) and i and lines[i - 1].strip() and not lines[i - 1].startswith("--")
                and "order by" not in text and "explain" not in text and "QUERY PLAN" not in lines[i - 1]):
            out.append(line)
            i += 1
            rows, row = [], []
            while i < len(lines) and not ROWS.match(lines[i]):
                row.append(lines[i])
                if not lines[i].endswith("+"):
                    rows.append(row)
                    row = []
                i += 1
            if row:
                rows.append(row)
            out += [part for row in sorted(rows) for part in row]
            statement = []
            continue
        statement = [] if ROWS.match(line) else statement + [line]
        out.append(line)
        i += 1
    return out


def read(path):
    """Returns a file's lines without their line ends."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\r\n") for line in f]


def reordered(build, name):
    """Returns the pg_regress tests of a meson test whose results differ from one of their expected files only in the
    order of rows no ORDER BY orders, read from the diffs it left."""
    diffs = os.path.join(build, "testrun", *name.split("/"), "regression.diffs")
    if not os.path.exists(diffs):
        return set()
    found = set()
    for header in filter(None, map(DIFF_HEADER.match, read(diffs))):
        expected, results = header.group(1), header.group(2)
        test = os.path.basename(results)[:-len(".out")]
        stem = os.path.join(os.path.dirname(expected), test)
        result = unordered(read(results))
        if any(unordered(read(path)) == result for path in [stem + ".out", *glob.glob(stem + "_*.out")]
               if os.path.exists(path)):
            found.add(test)
    return found


def meson(log):
    """Returns the tests and the completed meson tests of a `meson test` log."""
    tests, complete = {}, {}
    with open(log, encoding="utf-8") as f:
        for line in f:
            entry = json.loads(line)
            if entry["name"].startswith("setup - "):
                continue
            name = entry["name"].split(":", 1)[-1]
            stdout = (entry.get("stdout") or "").splitlines()
            statuses = [m for m in map(STATUS.match, stdout) if m]
            if statuses:
                passing = reordered(os.path.dirname(os.path.dirname(log)), name)
                for m in statuses:
                    tests[f"{name}:{m.group(2)}"] = "fail" if m.group(1) and m.group(2) not in passing else "ok"
                complete[name] = any(PLAN.match(line) for line in stdout)
            else:
                tests[name] = RESULTS.get(entry["result"], "fail")
                complete[name] = tests[name] != "fail"
    return tests, complete


def main():
    """Writes the results file for one log, or the file of a run whose suites weren't run."""
    args = [arg for arg in sys.argv[1:] if not arg.startswith("--")]
    if "--not-run" in sys.argv:
        tests, complete = {}, {}
    else:
        readers = {"--checks": checks, "--bats": bats, "--cargo": cargo}
        reader = next((readers[arg] for arg in sys.argv if arg in readers), meson)
        tests, complete = reader(args.pop(0))
    with open(args[0], "w", encoding="utf-8") as f:
        json.dump({"incomplete": "--incomplete" in sys.argv, "not_run": "--not-run" in sys.argv, "complete": complete,
                   "tests": tests}, f, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
