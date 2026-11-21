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
expected files is an order its statement leaves undefined passes, as Postgres' regression chapter's "Row Ordering
Differences" says: rows with no ORDER BY, rows tied under one, and the rows a LIMIT or OFFSET cuts from a tied run, as
Doltgrust's regression runner decides them. Each meson test, bats run, and test binary is marked
complete when it ran to its end (a failed meson test that printed no pg_regress results never did), so a comparison
can tell a test the branch removed from one a crash or a timeout never reached; --incomplete marks the whole run as
stopped before its end. --not-run records a run whose tree, by pgfork/targets.sh, builds no server, so its suites
were never meant to run. The setup suite's steps are left out."""

import difflib
import glob
import json
import os
import re
import sys

from pglast import ast, enums, parse_sql, parser

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
# psql's count of a table's rows, as `(3 rows)`.
ROW_COUNT = re.compile(r"^\(\d+ rows?\)$")
# The end of a COPY that reads its data from the script that follows it.
COPY_FROM = re.compile(r"\bfrom\s+stdin\b.*;\s*$|^\s*\\copy\b.*\bfrom\s+(?:stdin|pstdin)\b", re.IGNORECASE)
# A COPY to psql's output, its source, and its options.
COPY_TO = re.compile(r"^\s*copy\s+(.*?)\s+to\s+stdout\b(.*?);?\s*$", re.IGNORECASE | re.DOTALL)
# An option of a COPY to psql's output that is on unless it is followed by a false value.
COPY_OPTION = r"\b{}\b(?!\s*(?:=\s*)?(?:false|off|0)\b)"

# The orders a statement returns its rows in, as Doltgrust's harness decides them: `ANY` when Postgres returns them in
# no defined order, `DEFINED` for the one order of a statement that defines it, and `(keys, cut_start, cut_end)` for
# rows sorted by keys, each run of rows with equal keys in no defined order. A cut start or end means the first or last
# run may hold any of the tied rows, as an OFFSET or LIMIT that cuts the run picks. A key is the output column at an
# index, or `(name, index)`: the one output column of a name, or when no output column has the name, the column at an
# index whose expression is the key, as Postgres looks up a bare name in ORDER BY.
ANY, DEFINED = "any", "defined"

# The NOTICEs that functions of the regression tests raise once per row they read, each in a `notices` check: f_leak's,
# snoop's, and tattle's, a PL/pgSQL loop's rows, test_encoding's lines, and merge's row triggers, whose rows fire their
# BEFORE triggers and then their AFTER triggers in one order.
F_LEAK = ("notices", r"^NOTICE:  f_leak => ")
SNOOPED = ("notices", r"^NOTICE:  snooped value: ")
TATTLED = ("notices", r"^NOTICE:  x = \d+, y = \d+$")
RAISED_ROWS = ("notices", r"^NOTICE:  \d+$")
ENCODINGS = ("notices", r"^NOTICE:  (EUC_\w+|LATIN1|UTF8) ")
ROW_TRIGGERS = ("notices", r"^NOTICE:  (?P<phase>BEFORE|AFTER) (INSERT|UPDATE|DELETE) ROW trigger row: (?P<key>.*)$")

# The units of tests whose valid outputs are more than Postgres' expected output shows, as Doltgrust's regression
# runner lists them: by test and statement, how a received table with the expected table's columns and number of rows
# is checked. `some_of` takes different ones of every row Postgres returns for the statement without its LIMIT,
# recorded at the same point of its test, as LIMIT without ORDER BY picks any of them; `joined` takes a `bytea` value
# that is these values, in hex, in any order with the delimiter between them, as `string_agg` without ORDER BY joins
# them; `lines` takes the expected rows but for the order of the lines of each value, as a crosstab of `string_agg`
# values joined by newlines without ORDER BY shows them; and `notices` takes the lines a pattern matches, the NOTICEs a
# statement raises once per row it reads, in any order within each run of them, as `noticed` checks.
EXCEPTIONS = [
    ("with", "WITH t AS (\n    UPDATE y SET a = a * 100 RETURNING *\n)\nSELECT * FROM t LIMIT 10;",
     ("some_of", ["1100", "1200", "1300", "1400", "1500", "4200", "0", "-100", "-200", "-300", "-400", "-500", "-600",
                  "-700", "-800", "-900", "-1000", "0", "-100", "-200", "-300", "-400"])),
    ("aggregates", "select string_agg(v, '') from bytea_test_table;", ("joined", ["ff", "aa"], "")),
    ("aggregates", "select string_agg(v, NULL) from bytea_test_table;", ("joined", ["ff", "aa"], "")),
    ("aggregates", "select string_agg(v, decode('ee', 'hex')) from bytea_test_table;", ("joined", ["ff", "aa"], "ee")),
    ("psql_crosstab", "SELECT v, h, string_agg(c, E'\\n') FROM ctv_data GROUP BY v, h ORDER BY 1,2,3\n"
     " \\crosstabview 1 2 3", ("lines",)),
    ("psql_crosstab", "SELECT v,h, string_agg(c, E'\\n') AS c, row_number() OVER(ORDER BY h) AS r\n"
     "FROM ctv_data GROUP BY v, h ORDER BY 1,3,2\n \\crosstabview v h c r", ("lines",)),
    ("psql_crosstab", "SELECT v, h, string_agg(c, E'\\n') AS c, row_number() OVER(ORDER BY h DESC) AS r\n"
     "FROM ctv_data GROUP BY v, h ORDER BY 1,3,2\n \\crosstabview v h c r", ("lines",)),
    ("psql_crosstab", "SELECT v,h, string_agg(c, E'\\n') AS c, row_number() OVER(ORDER BY h NULLS LAST) AS r\n"
     "FROM ctv_data GROUP BY v, h ORDER BY 1,3,2\n \\crosstabview v h c r", ("lines",)),
    ("psql_crosstab", "SELECT v,h, string_agg(i::text, E'\\n') AS i FROM ctv_data\nGROUP BY v, h ORDER BY h,v\n"
     " \\crosstabview v h i", ("lines",)),
    ("psql_crosstab", "SELECT v,h,string_agg(i::text, E'\\n'), string_agg(c, E'\\n')\n"
     "FROM ctv_data GROUP BY v, h ORDER BY h,v\n \\crosstabview 2 1 4", ("lines",)),
    ("psql_crosstab", "SELECT v,h, string_agg(i::text, E'\\n') AS i, string_agg(c, E'\\n') AS c\n"
     "FROM ctv_data GROUP BY v, h ORDER BY h,v\n \\crosstabview 1 \"h\" 4", ("lines",)),
    ('rowsecurity', 'SELECT * FROM t1 WHERE f_leak(b);', F_LEAK),
    ('rowsecurity', 'SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;', F_LEAK),
    ('rowsecurity', 'SELECT * FROM part_document_satire WHERE f_leak(dtitle) ORDER BY did;', F_LEAK),
    ('rowsecurity', 'UPDATE t1 SET b = b WHERE f_leak(b) RETURNING *;', F_LEAK),
    ('rowsecurity', 'UPDATE t1 SET b = b WHERE f_leak(b) RETURNING tableoid::regclass, *, t1;', F_LEAK),
    ('rowsecurity', 'DELETE FROM t1 WHERE f_leak(b) RETURNING tableoid::regclass, *, t1;', F_LEAK),
    ('rowsecurity', 'SELECT * FROM bv1 WHERE f_leak(b);', F_LEAK),
    ('rowsecurity', 'SELECT * FROM z1 WHERE f_leak(b);', F_LEAK),
    ('rowsecurity', 'SELECT * FROM rls_view;', F_LEAK),
    ('rowsecurity', 'SELECT * FROM x1 WHERE f_leak(b) ORDER BY a ASC;', F_LEAK),
    ('rowsecurity', "UPDATE x1 SET b = b || '_updt' WHERE f_leak(b) RETURNING *;", F_LEAK),
    ('rowsecurity', 'DELETE FROM x1 WHERE f_leak(b) RETURNING *;', F_LEAK),
    ('rowsecurity', 'SELECT * FROM y2 WHERE f_leak(b);', F_LEAK),
    ('rowsecurity', 'SELECT * FROM y2 JOIN test_qual_pushdown ON (b = abc) WHERE f_leak(b);', F_LEAK),
    ('rowsecurity', 'WITH cte1 AS MATERIALIZED (SELECT * FROM t1 WHERE f_leak(b)) SELECT * FROM cte1;', F_LEAK),
    ('select_views', 'SELECT * FROM my_credit_card_normal WHERE f_leak(cnum);', F_LEAK),
    ('merge', 'MERGE INTO target t\nUSING source AS s\nON t.tid = s.sid\nWHEN MATCHED AND t.balance > s.delta THEN\n\tUPDATE SET balance = t.balance - s.delta\nWHEN MATCHED THEN\n\tDELETE\nWHEN NOT MATCHED THEN\n\tINSERT VALUES (s.sid, s.delta);', ROW_TRIGGERS),
    ('merge', 'MERGE INTO target t\nUSING source AS s\nON t.tid = s.sid\nWHEN MATCHED AND t.balance > s.delta THEN\n    UPDATE SET balance = t.balance - s.delta\nWHEN MATCHED THEN\n\tUPDATE SET balance = 0\nWHEN NOT MATCHED THEN\n    INSERT VALUES (s.sid, s.delta)\nWHEN NOT MATCHED BY SOURCE AND tid = 1 THEN\n\tUPDATE SET balance = 0\nWHEN NOT MATCHED BY SOURCE THEN\n\tDELETE\nRETURNING merge_action(), old, new, t.*;', ROW_TRIGGERS),
    ('merge', "DO $$\nDECLARE\n  result integer;\nBEGIN\nMERGE INTO target t\nUSING source AS s\nON t.tid = s.sid\nWHEN MATCHED AND s.sid = 3 THEN UPDATE SET balance = t.balance + s.delta\nWHEN MATCHED THEN DELETE\nWHEN NOT MATCHED THEN INSERT VALUES (sid, delta);\nIF FOUND THEN\n  RAISE NOTICE 'Found';\nELSE\n  RAISE NOTICE 'Not found';\nEND IF;\nGET DIAGNOSTICS result := ROW_COUNT;\nRAISE NOTICE 'ROW_COUNT = %', result;\nEND;\n$$;", ROW_TRIGGERS),
    ('merge', 'MERGE INTO target t1\nUSING target t2\nON t1.tid = t2.tid\nWHEN MATCHED THEN\n\tUPDATE SET balance = t1.balance + t2.balance\nWHEN NOT MATCHED THEN\n\tINSERT VALUES (t2.tid, t2.balance);', ROW_TRIGGERS),
    ('plancache', 'select cachebug();', RAISED_ROWS),
    ('encoding', 'SELECT COUNT(test_encoding(encoding, description, input)) > 0\nFROM encoding_tests;', ENCODINGS),
    ('updatable_views', 'SELECT * FROM rw_view1 WHERE snoop(person);', SNOOPED),
    ('updatable_views', 'UPDATE rw_view1 SET person=person WHERE snoop(person);', SNOOPED),
    ('updatable_views', 'DELETE FROM rw_view1 WHERE NOT snoop(person);', SNOOPED),
    ('updatable_views', "MERGE INTO rw_view1 t\n  USING (VALUES ('Tom'), ('Dick'), ('Harry')) AS v(person) ON t.person = v.person\n  WHEN MATCHED AND snoop(t.person) THEN UPDATE SET person = v.person;", SNOOPED),
    ('updatable_views', 'SELECT * FROM rw_view2 WHERE snoop(person);', SNOOPED),
    ('updatable_views', 'UPDATE rw_view2 SET person=person WHERE snoop(person);', SNOOPED),
    ('updatable_views', 'DELETE FROM rw_view2 WHERE NOT snoop(person);', SNOOPED),
    ('updatable_views', "MERGE INTO rw_view2 t\n  USING (VALUES ('Tom'), ('Dick'), ('Harry')) AS v(person) ON t.person = v.person\n  WHEN MATCHED AND snoop(t.person) THEN UPDATE SET person = v.person;", SNOOPED),
    ('updatable_views', 'DELETE FROM v1 WHERE snoop(a) AND leakproof(a); -- should not delete everything, just where a>5', SNOOPED),
    ('subselect', 'select * from\n  (select tattle(3, ten) as v, count(*) from tenk1 where unique1 < 3 group by 1) ss\n  where ss.v;', TATTLED),
]


def star(target):
    """Returns whether a target is `*` or a table's `*`."""
    return isinstance(target.val, ast.ColumnRef) and isinstance(target.val.fields[-1], ast.A_Star)


def key(node, targets):
    """Returns the output column a sort key names: by position, by output name, or by being the expression of a target
    before any `*` target; None when it names none."""
    column = None
    for i, target in enumerate(targets):
        if star(target):
            break
        if target.val == node:
            column = i
            break
    if isinstance(node, ast.A_Const):
        position = None if node.isnull or not isinstance(node.val, ast.Integer) else node.val.ival
        return position - 1 if position and position > 0 else None
    if isinstance(node, ast.ColumnRef) and len(node.fields) == 1 and isinstance(node.fields[0], ast.String):
        return (node.fields[0].sval, column)
    return column


def select_order(select):
    """Returns the order of a SELECT's rows: `ANY` without ORDER BY at its top level, sorted when every sort key names
    an output column, and otherwise `DEFINED`, since rows tied on a key that is not output can't be told apart from
    rows out of order."""
    if not select.sortClause:
        return ANY
    targets = select.targetList or () if select.op == enums.SetOperation.SETOP_NONE else ()
    keys = [key(sort.node, targets) if isinstance(sort, ast.SortBy) else None for sort in select.sortClause]
    if None in keys:
        return DEFINED
    count = select.limitCount
    limited = count is not None and not (isinstance(count, ast.A_Const) and count.isnull)
    return (keys, select.limitOffset is not None,
            limited and select.limitOption != enums.LimitOption.LIMIT_OPTION_WITH_TIES)


def statement_order(stmt):
    """Returns the order of a statement's rows: a SELECT's, `ANY` for an INSERT, UPDATE, DELETE, or MERGE with
    RETURNING, and `DEFINED` for any other statement."""
    if isinstance(stmt, ast.SelectStmt):
        return select_order(stmt)
    returning = isinstance(stmt, (ast.InsertStmt, ast.UpdateStmt, ast.DeleteStmt, ast.MergeStmt))
    return ANY if returning and stmt.returningList else DEFINED


class Statements:
    """Reads a script's units in order, remembering the order of the rows of each declared cursor and prepared
    statement."""

    def __init__(self):
        self.cursors, self.prepared = {}, {}

    def order(self, script):
        """Returns the order of the rows of a unit, the script psql read before its output, which is `DEFINED` unless
        one statement ends on the unit's last line, psql read no command there, and that statement returns rows in
        another order. A unit that copies to psql's output in no defined order returns `copy_order`'s order."""
        lines = ["" if line.lstrip().startswith("\\") else line for line in script]
        text = "\n".join(lines)
        last = len(text) - len(lines[-1]) if lines else 0
        try:
            parts = parser.split(text, with_parser=False, only_slices=True)
        except Exception:
            parts = [slice(0, len(text))]
        ending = []
        for part in parts:
            try:
                stmts = parse_sql(text[part])
            except Exception:
                stmts = ()
            for raw in stmts:
                if isinstance(raw.stmt, ast.DeclareCursorStmt):
                    self.cursors[raw.stmt.portalname] = statement_order(raw.stmt.query)
                elif isinstance(raw.stmt, ast.PrepareStmt):
                    self.prepared[raw.stmt.name] = statement_order(raw.stmt.query)
            if part.stop > last:
                ending.append((text[part], stmts))
        if script and script[-1].lstrip().startswith("\\copy"):
            copy = COPY_TO.match(script[-1].lstrip()[1:])
            return copy_order(copy) if copy else DEFINED
        if not script or script[-1].lstrip().startswith("\\") or len(ending) != 1:
            return DEFINED
        source, stmts = ending[0]
        copy = COPY_TO.match("\n".join(line for line in source.split("\n") if not line.lstrip().startswith("--")))
        if copy:
            return copy_order(copy)
        if len(stmts) != 1:
            return DEFINED
        stmt = stmts[0].stmt
        if isinstance(stmt, ast.FetchStmt) and not stmt.ismove:
            order = self.cursors.get(stmt.portalname, DEFINED)
            return (order[0], True, True) if isinstance(order, tuple) else order
        if isinstance(stmt, ast.ExecuteStmt):
            return self.prepared.get(stmt.name, DEFINED)
        return statement_order(stmt)


def copy_order(copy):
    """Returns how a COPY to psql's output orders its lines: `("copy", csv, header, array)`, in no defined order after
    a CSV header and inside a JSON array's brackets, when it copies a table or a query with no defined order, and
    otherwise `DEFINED`. Its options are read from its text, so that a format the parser doesn't know yet still
    counts."""
    source, options = copy.group(1).strip(), copy.group(2)
    if not source.startswith("("):
        source = re.sub(r"\(.*\)\s*$", "", source, flags=re.DOTALL)
    try:
        stmts = parse_sql(f"copy {source} to stdout")
    except Exception:
        return DEFINED
    stmt = stmts[0].stmt
    if stmt.query is not None and statement_order(stmt.query) != ANY:
        return DEFINED
    csv = re.search(r"\bcsv\b", options, re.IGNORECASE)
    header = bool(csv) and bool(re.search(COPY_OPTION.format("header"), options, re.IGNORECASE))
    array = bool(re.search(COPY_OPTION.format("force_array"), options, re.IGNORECASE))
    return ("copy", bool(csv), header, array)


def copied(order, expected, received):
    """Returns whether a COPY's received lines are its expected lines in another order, a JSON array's rows each
    keeping its own text after the comma or space before it, and a CSV header first; a CSV value over several lines
    keeps the order defined, since its lines can't be told apart from rows."""
    _, csv, header, array = order
    fixed = (1 if header else 0) + (1 if array else 0)
    if len(expected) != len(received) or expected[:fixed] != received[:fixed]:
        return False
    if array and (expected[-1:] != received[-1:] or len(expected) < 2):
        return False
    rows = lambda lines: sorted(line[1:] if array else line for line in lines[fixed:len(lines) - (1 if array else 0)])
    if csv and any(line.count('"') % 2 for line in expected):
        return False
    return rows(expected) == rows(received)


def row_count(line):
    """Returns whether a line is psql's count of a table's rows."""
    return bool(ROW_COUNT.match(line))


def continues(separator, line):
    """Returns whether a row's line ends with a value that continues on the next line, which psql marks with a `+` in
    place of the space before a column's `|` or at the end of the last column's width."""
    if len(line) == len(separator) and line.endswith("+"):
        return True
    return any(p > 0 and line[p - 1:p + 1] == "+|" for p, c in enumerate(separator) if c == "+")


def table(lines):
    """Returns an output's lines through the table header's separator, the lines of each row, and its lines from the
    row count on, or None when it holds no table."""
    separator = next((i for i, line in enumerate(lines)
                      if all(part and set(part) == {"-"} for part in line.split("+"))), None)
    if not separator:
        return None
    count = next((i for i in range(separator + 1, len(lines)) if row_count(lines[i])), None)
    if count is None:
        return None
    rows, row = [], []
    for line in lines[separator + 1:count]:
        row.append(line)
        if not continues(lines[separator], line):
            rows.append(row)
            row = []
    return None if row else (lines[:separator + 1], rows, lines[count:])


def cells(head, rows):
    """Returns each row's lines and the text of its values, split at the columns of a table's head, whose last line is
    the separator under the column names; a row's values are empty when a line has no `|` where a column ends. A
    value's text leaves out the last place of its column, where psql marks a value that continues on the next line."""
    separator = head[-1]
    ends = [i for i, c in enumerate(separator) if c == "+"]
    result = []
    for lines in rows:
        values = [[] for _ in range(len(ends) + 1)]
        if any(line[end:end + 1] != "|" for line in lines for end in ends):
            result.append((lines, []))
            continue
        for line in lines:
            for value, start, end in zip(values, [0] + [end + 1 for end in ends], ends + [len(separator)]):
                value.append(line[start:end - 1].strip())
        result.append((lines, ["\n".join(parts) for parts in values]))
    return result


def columns(keys, names):
    """Returns the output columns that sort keys name among a result's column names, or None when one names no single
    column."""
    found = []
    for k in keys:
        if isinstance(k, tuple):
            name, expression = k
            count = names.count(name)
            k = expression if count == 0 else names.index(name) if count == 1 else None
        if k is None or k >= len(names):
            return None
        found.append(k)
    return found


def valid(order, expected, received):
    """Returns whether two outputs in psql's aligned format hold the same table but for an order of its rows that a
    statement's order allows, each row keeping the lines that continue its values."""
    mine, theirs = table(expected), table(received)
    if not mine or not theirs or mine[0] != theirs[0] or mine[2] != theirs[2] or len(mine[1]) != len(theirs[1]):
        return False
    head = mine[0]
    rows, other = cells(head, mine[1]), cells(head, theirs[1])
    names = cells(head, [[head[-2]]])[0][1] if all(values for _, values in rows + other) else []
    if order == ANY:
        return sorted(lines for lines, _ in rows) == sorted(lines for lines, _ in other)
    keys = columns(order[0], names) if isinstance(order, tuple) else None
    if keys is None:
        return False
    _, cut_start, cut_end = order
    start = 0
    while start < len(rows):
        end = start + 1
        while end < len(rows) and all(rows[end][1][k] == rows[start][1][k] for k in keys):
            end += 1
        if (cut_start and start == 0) or (cut_end and end == len(rows)):
            if any(values[k] != rows[start][1][k] for _, values in other[start:end] for k in keys):
                return False
        elif sorted(lines for lines, _ in rows[start:end]) != sorted(lines for lines, _ in other[start:end]):
            return False
        start = end
    return True


def joined(text, values, delimiter):
    """Returns whether a text is the values in some order with the delimiter between them."""
    for i, value in enumerate(values):
        rest = values[:i] + values[i + 1:]
        if text.startswith(value):
            after = text[len(value):]
            if not rest and not after or rest and after.startswith(delimiter) and joined(
                    after[len(delimiter):], rest, delimiter):
                return True
    return False


def noticed(pattern, order, expected, received):
    """Returns whether two outputs differ only in the order of the lines a pattern matches within each run of them, the
    rest equal or in an order the statement allows. The matched lines keep their places, each run (consecutive matched
    lines of one `phase` group) holds the same lines, and the rows a `key` group names keep one order across runs, as a
    statement's BEFORE and AFTER row triggers fire for its rows in one order."""
    match = re.compile(pattern).match
    mine = [i for i, line in enumerate(expected) if match(line)]
    if mine != [i for i, line in enumerate(received) if match(line)]:
        return False
    rest = lambda lines: [line for i, line in enumerate(lines) if i not in set(mine)]
    if rest(expected) != rest(received) and not valid(order, rest(expected), rest(received)):
        return False
    group = lambda line, name: match(line).groupdict().get(name)
    runs = []
    for i in mine:
        if runs and runs[-1][-1] == i - 1 and group(expected[i], "phase") == group(expected[i - 1], "phase"):
            runs[-1].append(i)
        else:
            runs.append([i])
    before = set()
    for run in runs:
        lines = [received[i] for i in run]
        if sorted(lines) != sorted(expected[i] for i in run):
            return False
        keys = [group(line, "key") for line in lines]
        if any(group(line, "phase") != group(expected[run[0]], "phase") for line in lines):
            return False
        pairs = [(earlier, later) for j, later in enumerate(keys) for earlier in keys[:j]
                 if None not in (earlier, later) and earlier != later]
        if any((later, earlier) in before for earlier, later in pairs):
            return False
        before.update(pairs)
    return True


def excepted(test, script, expected, received, order=DEFINED):
    """Returns whether a unit of a test, whose statement returns rows in an order, is one of `EXCEPTIONS` and passes its
    check: `notices` by `noticed`, and every other check only when the received table has the expected table's columns
    and number of rows."""
    query = "\n".join(script)
    check = next((valid for name, text, valid in EXCEPTIONS if name == test and query.endswith(text)), None)
    if check and check[0] == "notices":
        return noticed(check[1], order, expected, received)
    mine, theirs = table(expected), table(received)
    if not check or not mine or not theirs:
        return False
    names = lambda head: cells(head, [[head[-2]]])[0][1]
    if (mine[0][:-2] != theirs[0][:-2] or names(mine[0]) != names(theirs[0]) or mine[2] != theirs[2]
            or len(mine[1]) != len(theirs[1])):
        return False
    rows, other = cells(mine[0], mine[1]), cells(theirs[0], theirs[1])
    if check[0] == "some_of":
        full = list(check[1])
        for _, values in other:
            if len(values) != 1 or values[0] not in full:
                return False
            full.remove(values[0])
        return True
    if check[0] == "joined":
        return (len(other) == 1 and len(other[0][1]) == 1 and other[0][1][0].startswith("\\x")
                and joined(other[0][1][0][2:], check[1], check[2]))
    return all(len(a) == len(b) and all(sorted(x.split("\n")) == sorted(y.split("\n")) for x, y in zip(a, b))
               for (_, a), (_, b) in zip(rows, other))


def units(script, output):
    """Splits a test's output into units, each the script's lines psql echoed before a stretch of output and that
    output. The script's blank lines are left out, since psql's own blank lines can't be told apart from them, and so
    is the data a COPY reads from the script, which psql doesn't echo."""
    sql, data = [], False
    for line in script:
        if data:
            data = line != "\\."
        elif line.strip():
            sql.append(line)
            data = bool(COPY_FROM.search(line))
    echoed = set()
    for block in difflib.SequenceMatcher(None, sql, output, autojunk=False).get_matching_blocks():
        echoed.update(range(block.b, block.b + block.size))
    found = []
    for i, line in enumerate(output):
        if i in echoed:
            if not found or found[-1][1]:
                found.append(([], []))
            found[-1][0].append(line)
        elif found:
            found[-1][1].append(line)
        else:
            found.append(([], [line]))
    return found


def same(test, script, expected, received):
    """Returns whether a test's results differ from an expected output only in orders its statements leave undefined."""
    mine, theirs = units(script, expected), units(script, received)
    if [lines for lines, _ in mine] != [lines for lines, _ in theirs]:
        return False
    statements = Statements()
    for (lines, out), (_, other) in zip(mine, theirs):
        order = statements.order(lines)
        if out == other:
            continue
        if isinstance(order, tuple) and order[0] == "copy":
            if not copied(order, out, other):
                return False
        elif not (valid(order, out, other) or excepted(test, lines, out, other, order)):
            return False
    return True


def read(path):
    """Returns a file's lines without their line ends."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\r\n") for line in f]


def reordered(build, name):
    """Returns the pg_regress tests of a meson test whose results differ from one of their expected files only in
    orders their statements leave undefined, read from the diffs it left."""
    diffs = os.path.join(build, "testrun", *name.split("/"), "regression.diffs")
    if not os.path.exists(diffs):
        return set()
    found = set()
    for header in filter(None, map(DIFF_HEADER.match, read(diffs))):
        expected, results = header.group(1), header.group(2)
        test = os.path.basename(results)[:-len(".out")]
        stem = os.path.join(os.path.dirname(expected), test)
        script = os.path.join(os.path.dirname(os.path.dirname(expected)), "sql", test + ".sql")
        if not os.path.exists(script):
            continue
        sql, result = read(script), read(results)
        if any(same(test, sql, read(path), result) for path in [stem + ".out", *glob.glob(stem + "_*.out")]
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
