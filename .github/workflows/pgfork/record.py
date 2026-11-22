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
"""record.py <regress dir> <bindir> <dlpath> <test>: records an alternative expected output of a pg_regress test, the
output stock Postgres gives when the test's deciding tables hold their rows in the order the fork reads them, into
alternatives/<test>_1.out beside this script, for results.py to accept as Postgres' regression chapter accepts any
expected file. alternatives/<test>.json lists the reorders: before which line of the script (its `occurrence`, 1 by
default), which table, and the ORDER BY that gives the fork's order. Each reorder is the one step `reorder` writes,
hidden from psql's echo, which only puts the table's rows in that order: it fails the recording if any table's rows or
any sequence's state differ after it. A temporary instance of the Postgres at bindir, with dlpath holding its build of
the regression library, runs the parallel schedule's tests before this one, one at a time, then the changed test; the
recording is refused unless every earlier test matches its expected file. The one line psql echoes of each step,
`\\set ECHO none`, is the recording's only change to Postgres' output."""

import json
import os
import shutil
import subprocess
import sys
import tempfile

# The line psql echoes when a reorder begins.
HIDDEN = "\\set ECHO none"
# The line a reorder prints when the rows or sequences it saw differ after it.
REFUSED = "record.py: the reorder changed the database"
# A function of the session that digests every user table's rows as a multiset and every sequence's state.
DIGEST = """CREATE FUNCTION pg_temp.record_digest() RETURNS text LANGUAGE plpgsql AS $record$
DECLARE r record; d text := ''; x text;
BEGIN
  FOR r IN SELECT c.oid::regclass AS t, c.relkind FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
           WHERE c.relkind IN ('r', 'm', 'S') AND n.nspname NOT IN ('pg_catalog', 'information_schema')
             AND n.nspname NOT LIKE 'pg_toast%' ORDER BY c.oid LOOP
    IF r.relkind = 'S' THEN
      EXECUTE format('SELECT last_value::text || is_called::text FROM %s', r.t) INTO x;
    ELSE
      EXECUTE format('SELECT md5(coalesce(string_agg(x::text, %L ORDER BY x::text), %L)) FROM %s x', ',', '', r.t)
        INTO x;
    END IF;
    d := d || r.t::text || ':' || coalesce(x, '') || ';';
  END LOOP;
  RETURN md5(d);
END $record$;"""


def reorder(table, order):
    """Returns the lines of the one step that puts a table's rows in an order: with no user trigger, rule, or foreign
    key firing (session_replication_role replica), it copies the rows, deletes them, and inserts the copy in the
    order, then checks that the order gave every distinct row a place of its own, so that none falls back to the heap's
    order, and that every table's rows and every sequence's state are what they were."""
    return [HIDDEN, "\\o /dev/null", DIGEST, "SELECT pg_temp.record_digest() AS record_before \\gset",
            "SET session_replication_role = replica;",
            f"CREATE TEMP TABLE record_rows AS SELECT * FROM {table};",
            "SELECT coalesce(bool_and(record_count = 1), true) AS record_total FROM (SELECT count(DISTINCT "
            "record_row::text) AS record_count FROM (SELECT record_rows AS record_row, rank() OVER "
            f"(ORDER BY {order}) AS record_rank FROM record_rows) record_ranked GROUP BY record_rank) record_ranks"
            " \\gset",
            f"DELETE FROM {table};",
            f"INSERT INTO {table} OVERRIDING SYSTEM VALUE SELECT * FROM record_rows ORDER BY {order};",
            "DROP TABLE record_rows;", "RESET session_replication_role;",
            "SELECT pg_temp.record_digest() = :'record_before' AS record_same \\gset",
            "DROP FUNCTION pg_temp.record_digest();", "\\o",
            f"\\if :record_same\n\\if :record_total\n\\else\n\\warn '{REFUSED}'\n\\endif\n"
            f"\\else\n\\warn '{REFUSED}'\n\\endif",
            "\\set ECHO all"]


def schedule(regress, test):
    """Returns the parallel schedule's tests that come before a test, in order, and the test itself."""
    before = []
    with open(os.path.join(regress, "parallel_schedule"), encoding="utf-8") as f:
        for line in f:
            if not line.startswith("test:"):
                continue
            for name in line.split()[1:]:
                if name == test:
                    return before + [test]
                before.append(name)
    sys.exit(f"{test} is not in the parallel schedule")


def changed(script, reorders):
    """Returns a script's lines with each reorder's step before the line it names."""
    lines = script.split("\n")
    for step in reorders:
        if not set(step) <= {"before", "occurrence", "table", "order"} or not {"before", "table", "order"} <= set(step):
            sys.exit(f"a reorder names only before, occurrence, table, and order: {step}")
        found = [i for i, line in enumerate(lines) if line == step["before"]]
        if len(found) < step.get("occurrence", 1):
            sys.exit(f"no line {step['before']!r} to reorder before")
        at = found[step.get("occurrence", 1) - 1]
        lines[at:at] = "\n".join(reorder(step["table"], step["order"])).split("\n")
    return "\n".join(lines)


def main():
    """Records one test's alternative expected output."""
    regress, bindir, dlpath, test = sys.argv[1:5]
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "alternatives")
    with open(os.path.join(here, test + ".json"), encoding="utf-8") as f:
        reorders = json.load(f)
    with tempfile.TemporaryDirectory() as work:
        inputs = os.path.join(work, "in")
        for part in ("sql", "expected", "data", "input", "output"):
            if os.path.isdir(os.path.join(regress, part)):
                shutil.copytree(os.path.join(regress, part), os.path.join(inputs, part))
        with open(os.path.join(inputs, "expected", test + ".out"), encoding="utf-8") as f:
            if HIDDEN in f.read().split("\n"):
                sys.exit(f"{test}'s expected file already holds `{HIDDEN}`")
        path = os.path.join(inputs, "sql", test + ".sql")
        with open(path, encoding="utf-8") as f:
            script = f.read()
        with open(path, "w", encoding="utf-8") as f:
            f.write(changed(script, reorders))
        with open(os.path.join(work, "schedule"), "w", encoding="utf-8") as f:
            f.writelines(f"test: {name}\n" for name in schedule(regress, test))
        output = os.path.join(work, "out")
        subprocess.run([os.path.join(bindir, "..", "lib", "pgxs", "src", "test", "regress", "pg_regress"),
                        f"--temp-instance={os.path.join(work, 'instance')}", f"--inputdir={inputs}",
                        f"--outputdir={output}", f"--dlpath={dlpath}", f"--bindir={bindir}",
                        f"--schedule={os.path.join(work, 'schedule')}"], check=False, stdout=subprocess.DEVNULL)
        with open(os.path.join(output, "regression.out"), encoding="utf-8") as f:
            failed = [line for line in f if line.startswith("not ok") and line.split()[4] != test]
        if failed:
            sys.exit("earlier tests failed, so the recording is refused:\n" + "".join(failed))
        with open(os.path.join(output, "results", test + ".out"), encoding="utf-8") as f:
            recorded = f.read().split("\n")
        with open(os.path.join(inputs, "expected", test + ".out"), encoding="utf-8") as f:
            errors = sum(line.startswith("ERROR:") for line in f.read().split("\n"))
        if (any(REFUSED in line for line in recorded) or recorded.count(HIDDEN) != len(reorders)
                or sum(line.startswith("ERROR:") for line in recorded) != errors):
            sys.exit("a reorder changed the database or failed, so the recording is refused")
        with open(os.path.join(here, test + "_1.out"), "w", encoding="utf-8") as f:
            f.write("\n".join(line for line in recorded if line != HIDDEN))


if __name__ == "__main__":
    main()
