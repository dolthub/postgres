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
"""test_results.py: tests of results.py's rule for the orders a statement leaves undefined, ported from Doltgrust's
harness and regression runner (crates/harness/src/order.rs and crates/regress/src/order.rs), with tests of the COPY
rule only this runner has."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import results  # noqa: E402
from results import ANY, DEFINED, Statements, columns, copied, excepted, joined, noticed, same, valid  # noqa: E402


def sorted_by(keys, cut_start=False, cut_end=False):
    """Returns a sorted order by keys."""
    return (keys, cut_start, cut_end)


def render(rows, names=("k", "v")):
    """Returns the psql table of rows of one-character values under one-character column names."""
    head = [" " + " | ".join(names) + " ", "+".join("---" for _ in names)]
    return head + [" " + " | ".join(row) for row in rows] + [f"({len(rows)} {'row' if len(rows) == 1 else 'rows'})", ""]


def order(query, statements=None):
    """Returns the order of a one-line unit."""
    return (statements or Statements()).order([query])


class Orders(unittest.TestCase):
    """The orders of statements, as Doltgrust's harness decides them."""

    def test_unordered_statements(self):
        statements = Statements()
        for query, expected in [
            ("SELECT * FROM t;", ANY),
            ("SELECT * FROM t ORDER BY random();", DEFINED),
            ("SELECT * FROM (SELECT * FROM t ORDER BY a) s;", ANY),
            ("SELECT a FROM t UNION SELECT b FROM u ORDER BY 1;", sorted_by([0])),
            ("WITH w AS (SELECT 1) SELECT * FROM w;", ANY),
            ("SELECT array_agg(a ORDER BY b) FROM t;", ANY),
            ("EXPLAIN SELECT * FROM t;", DEFINED),
            ("SELECT 1; SELECT 2;", DEFINED),
            ("INSERT INTO t VALUES (1) RETURNING *;", ANY),
            ("DELETE FROM t;", DEFINED),
            ("SELECT :x;", DEFINED),
            ("SELECT * FROM t LIMIT 2;", ANY),
            ("DECLARE c CURSOR FOR SELECT * FROM t;", DEFINED),
            ("FETCH ALL FROM c;", ANY),
            ("MOVE ALL IN c;", DEFINED),
            ("DECLARE c CURSOR FOR SELECT * FROM t ORDER BY a;", DEFINED),
            ("FETCH 2 FROM c;", sorted_by([("a", None)], True, True)),
            ("FETCH ALL FROM d;", DEFINED),
            ("PREPARE p AS SELECT * FROM t;", DEFINED),
            ("EXECUTE p;", ANY),
            ("EXECUTE q;", DEFINED),
        ]:
            self.assertEqual(order(query, statements), expected, query)

    def test_sort_keys_resolve_to_output_columns(self):
        for query, expected in [
            ("SELECT a, b FROM t ORDER BY 2, 1;", sorted_by([1, 0])),
            ("SELECT a AS x, b FROM t ORDER BY a;", sorted_by([("a", 0)])),
            ("SELECT a + 1, b FROM t ORDER BY a + 1 DESC;", sorted_by([0])),
            ("SELECT a, b FROM t ORDER BY t.b;", DEFINED),
            ("SELECT t.a, b FROM t ORDER BY t.a;", sorted_by([0])),
            ("SELECT *, a + 1 FROM t ORDER BY a + 1;", DEFINED),
            ("SELECT a FROM t ORDER BY b + 1;", DEFINED),
            ("VALUES (1), (2) ORDER BY 1;", sorted_by([0])),
        ]:
            self.assertEqual(order(query), expected, query)
        self.assertEqual(columns([("x", None)], ["a", "x"]), [1])
        self.assertEqual(columns([("a", 0)], ["x", "b"]), [0])
        self.assertIsNone(columns([("a", None)], ["x", "b"]))
        self.assertIsNone(columns([("a", 0)], ["a", "a"]))
        self.assertIsNone(columns([2], ["a", "b"]))

    def test_limits_cut_the_last_run_unless_with_ties(self):
        for query, expected in [
            ("SELECT a FROM t ORDER BY a LIMIT 2;", sorted_by([("a", 0)], False, True)),
            ("SELECT a FROM t ORDER BY a LIMIT ALL;", sorted_by([("a", 0)])),
            ("SELECT a FROM t ORDER BY a OFFSET 1;", sorted_by([("a", 0)], True, False)),
            ("SELECT a FROM t ORDER BY a FETCH FIRST 2 ROWS ONLY;", sorted_by([("a", 0)], False, True)),
            ("SELECT a FROM t ORDER BY a FETCH FIRST 2 ROWS WITH TIES;", sorted_by([("a", 0)])),
            ("SELECT a FROM t ORDER BY a OFFSET 1 FETCH FIRST 2 ROWS WITH TIES;", sorted_by([("a", 0)], True, False)),
        ]:
            self.assertEqual(order(query), expected, query)

    def test_a_unit_takes_the_order_of_the_statement_on_its_last_line(self):
        self.assertEqual(Statements().order(["CREATE TABLE t (a int);", "SELECT * FROM t;"]), ANY)
        self.assertEqual(Statements().order(["SELECT * FROM t;", "\\gset"]), DEFINED)
        self.assertEqual(Statements().order(["SELECT * FROM t", "ORDER BY a;"]), sorted_by([("a", None)]))
        self.assertEqual(Statements().order(["SELECT * FROM t; SELECT * FROM u;"]), DEFINED)


class Tables(unittest.TestCase):
    """Received tables checked against expected ones in an order."""

    def test_ties_come_in_any_order(self):
        rows = [("1", "a"), ("1", "b"), ("2", "c"), ("3", "d"), ("3", "e")]
        by_k = sorted_by([0])
        self.assertTrue(valid(by_k, render(rows), render([rows[1], rows[0], rows[2], rows[4], rows[3]])))
        self.assertFalse(valid(by_k, render(rows), render([rows[0], rows[2], rows[1], rows[3], rows[4]])))
        self.assertFalse(valid(by_k, render(rows), render(rows[:4] + [("3", "f")])))
        self.assertFalse(valid(DEFINED, render(rows), render([rows[1], rows[0], rows[2], rows[3], rows[4]])))
        self.assertTrue(valid(ANY, render(rows), render([rows[4], rows[1], rows[2], rows[3], rows[0]])))

    def test_unresolved_keys_keep_the_order(self):
        rows = [("1", "a"), ("1", "b")]
        self.assertFalse(valid(sorted_by([("z", None)]), render(rows), render(rows[::-1])))

    def test_cut_runs_may_hold_other_tied_rows(self):
        expected = render([("1", "a"), ("2", "b"), ("2", "c")])
        received = [("1", "a"), ("2", "c"), ("2", "z")]
        self.assertTrue(valid(sorted_by([0], False, True), expected, render(received)))
        self.assertFalse(valid(sorted_by([0]), expected, render(received)))
        self.assertFalse(valid(sorted_by([0], False, True), expected, render([("1", "a"), ("2", "c"), ("3", "z")])))
        self.assertFalse(valid(sorted_by([0], False, True), expected, render(received[:2])))
        self.assertTrue(valid(sorted_by([0], True, False), expected, render([("1", "y"), ("2", "b"), ("2", "c")])))
        self.assertFalse(valid(sorted_by([0], True, False), expected, render([("1", "y"), ("2", "b"), ("2", "d")])))

    def test_fetches_from_sorted_cursors_cut_both_ends(self):
        statements = Statements()
        statements.order(["DECLARE c CURSOR FOR SELECT k, v FROM t ORDER BY k;"])
        fetch = statements.order(["FETCH 3 FROM c;"])
        expected = render([("1", "a"), ("2", "b"), ("2", "c"), ("3", "d")])
        self.assertTrue(valid(fetch, expected, render([("1", "x"), ("2", "c"), ("2", "b"), ("3", "y")])))
        self.assertFalse(valid(fetch, expected, render([("1", "x"), ("2", "c"), ("2", "z"), ("3", "y")])))

    def test_reordered_tables(self):
        expected = [" p  | q  ", "----+----", " a +| x+", " bb | ", " c  | d +", "    | e", " f+ | g", "(3 rows)", ""]
        moved = [" p  | q  ", "----+----", " c  | d +", "    | e", " f+ | g", " a +| x+", " bb | ", "(3 rows)", ""]
        split = [" p  | q  ", "----+----", " a +| x+", "    | e", " f+ | g", " c  | d +", " bb | ", "(3 rows)", ""]
        self.assertTrue(valid(ANY, expected, moved))
        self.assertFalse(valid(ANY, expected, split))
        self.assertFalse(valid(ANY, expected, [line.replace("f+", "f-") for line in moved]))
        self.assertFalse(valid(ANY, expected, [line.replace("(3 rows)", "(4 rows)") for line in moved]))
        self.assertFalse(valid(ANY, ["NOTICE:  n"] + expected, moved))
        self.assertTrue(valid(ANY, [" a ", "---", " 1", " 2", "(2 rows)"], [" a ", "---", " 2", " 1", "(2 rows)"]))
        self.assertFalse(valid(ANY, [" 1", " 2"], [" 2", " 1"]))

    def test_a_value_ending_in_a_plus_sign_is_a_row_of_its_own(self):
        expected = [" to_char ", "---------", " 123456+", " 654321+", "(2 rows)", ""]
        self.assertTrue(valid(ANY, expected, [expected[0], expected[1], expected[3], expected[2], *expected[4:]]))
        self.assertFalse(valid(ANY, expected, [expected[0], expected[1], expected[3], " 123457+", *expected[4:]]))

    def test_sorted_tables_take_ties_in_any_order(self):
        expected, swapped = render([("1", "a"), ("1", "b"), ("2", "c")]), render([("1", "b"), ("1", "a"), ("2", "c")])
        self.assertTrue(valid(sorted_by([0]), expected, swapped))
        self.assertTrue(valid(sorted_by([("k", None)]), expected, swapped))
        self.assertFalse(valid(sorted_by([("v", None)]), expected, swapped))
        self.assertFalse(valid(sorted_by([("z", None)]), expected, swapped))
        self.assertFalse(valid(DEFINED, expected, swapped))
        wide = [" k  | v ", "----+---", " 日 | a", " 日 | b", "(2 rows)", ""]
        self.assertFalse(valid(sorted_by([0]), wide, [wide[0], wide[1], wide[3], wide[2], wide[4], wide[5]]))


class Exceptions(unittest.TestCase):
    """The units whose valid outputs are more than Postgres' expected output shows."""

    def test_limited_rows_are_some_of_the_full_rows(self):
        query = results.EXCEPTIONS[0][1].split("\n")
        table = lambda rows: ["   a   ", "-------", *[f" {row:>5}" for row in rows], f"({len(rows)} rows)", ""]
        expected = table(["1100", "1200", "1300", "1400", "1500", "4200", "0", "-100", "-200", "-300"])
        other = ["-500", "-100", "-100", "1400", "-600", "-300", "-300", "4200", "1300", "-800"]
        self.assertTrue(excepted("with", query, expected, table(other)))
        self.assertFalse(excepted("with", query, expected, table([v.replace("-800", "-801") for v in other])))
        self.assertFalse(excepted("with", query, expected, table(["-100" if v == "-500" else v for v in other])))
        self.assertFalse(excepted("with", query, expected, table(["-500"])))
        self.assertFalse(excepted("union", query, expected, table(other)))

    def test_joined_values_come_in_any_order(self):
        query = ["select string_agg(v, decode('ee', 'hex')) from bytea_test_table;"]
        table = lambda value: [" string_agg ", "------------", f" {value}", "(1 row)", ""]
        self.assertTrue(excepted("aggregates", query, table("\\xffeeaa"), table("\\xaaeeff")))
        self.assertFalse(excepted("aggregates", query, table("\\xffeeaa"), table("\\xaaff")))
        self.assertFalse(excepted("aggregates", query, table("\\xffeeaa"), table("\\xaaeeaa")))
        self.assertTrue(joined("ffaa", ["ff", "aa"], "") and joined("aaff", ["ff", "aa"], ""))
        self.assertFalse(joined("ffaaaa", ["ff", "aa"], ""))

    def test_an_exception_is_its_units_last_statement(self):
        query = "select string_agg(v, decode('ee', 'hex')) from bytea_test_table;"
        table = lambda value: [" string_agg ", "------------", f" {value}", "(1 row)", ""]
        expected, received = table("\\xffeeaa"), table("\\xaaeeff")
        self.assertTrue(excepted("aggregates", ["insert into bytea_test_table values (1);", query], expected, received))
        self.assertFalse(excepted("aggregates", [query + " select 1;"], expected, received))
        self.assertFalse(excepted("aggregates", [query[:-1] + " limit 1;"], expected, received))

    def test_crosstab_values_take_their_lines_in_any_order(self):
        query = results.EXCEPTIONS[4][1].split("\n")
        expected = [" v  | h1  |  h2  ", "----+-----+------", " v0 | qux+| foo +", "    | dbl | quux ", " v1 |     | baz",
                    "(2 rows)", ""]
        other = [" v  | h1  |  h2  ", "----+-----+------", " v0 | dbl+| quux+", "    | qux | foo  ", " v1 |     | baz",
                 "(2 rows)", ""]
        self.assertTrue(excepted("psql_crosstab", query, expected, other))
        self.assertFalse(excepted("psql_crosstab", query, expected, [line.replace("dbl", "dbx") for line in other]))
        self.assertFalse(excepted("psql_crosstab", query, expected, [line.replace(" baz", " bay") for line in other]))

    def test_row_notices_come_in_any_order_within_their_run(self):
        f_leak = results.F_LEAK[1]
        rows = ["NOTICE:  f_leak => a", "NOTICE:  f_leak => b", "NOTICE:  f_leak => c"]
        expected = rows + render([("1", "a"), ("2", "b")])
        received = [rows[2], rows[0], rows[1]] + render([("2", "b"), ("1", "a")])
        self.assertTrue(noticed(f_leak, ANY, expected, received))
        self.assertFalse(noticed(f_leak, DEFINED, expected, received))
        self.assertFalse(noticed(f_leak, ANY, expected, [rows[2], rows[0], rows[0]] + received[3:]))
        self.assertFalse(noticed(f_leak, ANY, expected, [rows[2], rows[0]] + received[3:]))
        self.assertFalse(noticed(f_leak, ANY, expected, received[:3] + render([("2", "b"), ("1", "x")])))
        self.assertFalse(noticed(f_leak, ANY, expected, received[1:3] + render([("2", "b")])[:2] + [rows[0]]
                                 + render([("2", "b"), ("1", "a")])[2:]))
        self.assertTrue(excepted("rowsecurity", ["SELECT * FROM t1 WHERE f_leak(b);"], expected, received, ANY))
        self.assertFalse(excepted("rowsecurity", ["SELECT * FROM t2 WHERE f_leak(b);"], expected, received, ANY))

    def test_unmatched_notices_keep_their_places(self):
        f_leak = results.F_LEAK[1]
        expected = ["NOTICE:  f_leak => a", "NOTICE:  f_leak => b", "NOTICE:  other", "NOTICE:  f_leak => c"]
        self.assertTrue(noticed(f_leak, DEFINED, expected, [expected[1], expected[0], expected[2], expected[3]]))
        self.assertFalse(noticed(f_leak, DEFINED, expected, [expected[3], expected[1], expected[2], expected[0]]))
        self.assertFalse(noticed(f_leak, DEFINED, expected, [expected[2], expected[1], expected[0], expected[3]]))
        self.assertFalse(noticed(f_leak, DEFINED, expected, [*expected[:2], "NOTICE:  another", expected[3]]))

    def test_row_triggers_keep_their_phases_and_one_row_order(self):
        triggers = results.ROW_TRIGGERS[1]
        line = lambda phase, row: f"NOTICE:  {phase} UPDATE ROW trigger row: {row}"
        statement = "NOTICE:  BEFORE UPDATE STATEMENT trigger"
        expected = [statement] + [line(p, r) for p in ("BEFORE", "AFTER") for r in ("(1,1)", "(2,2)", "(3,3)")]
        reordered = [statement] + [line(p, r) for p in ("BEFORE", "AFTER") for r in ("(3,3)", "(1,1)", "(2,2)")]
        self.assertTrue(noticed(triggers, DEFINED, expected, reordered))
        mixed = [statement] + [line(p, r) for p, r in [("BEFORE", "(3,3)"), ("BEFORE", "(1,1)"), ("BEFORE", "(2,2)"),
                                                       ("AFTER", "(1,1)"), ("AFTER", "(3,3)"), ("AFTER", "(2,2)")]]
        self.assertFalse(noticed(triggers, DEFINED, expected, mixed))
        crossed = [statement] + [line(p, r) for p, r in [("BEFORE", "(1,1)"), ("BEFORE", "(2,2)"), ("AFTER", "(3,3)"),
                                                         ("BEFORE", "(3,3)"), ("AFTER", "(1,1)"), ("AFTER", "(2,2)")]]
        self.assertFalse(noticed(triggers, DEFINED, expected, crossed))
        self.assertFalse(noticed(triggers, DEFINED, expected, [reordered[1]] + [statement] + reordered[2:]))



class Copies(unittest.TestCase):
    """COPY to psql's output, whose lines come in no defined order when its source has none."""

    def test_a_csv_header_stays_first(self):
        copy = order("copy t to stdout csv header;")
        expected = ["a,b", "1,x", "2,y"]
        self.assertTrue(copied(copy, expected, ["a,b", "2,y", "1,x"]))
        self.assertFalse(copied(copy, expected, ["2,y", "a,b", "1,x"]))
        self.assertFalse(copied(copy, expected, ["a,b", "2,y", "1,z"]))
        self.assertFalse(copied(copy, ['a,b', '1,"x', 'y"'], ['a,b', 'y"', '1,"x']))

    def test_a_json_array_keeps_its_brackets(self):
        copy = order("copy t to stdout (format json, force_array);")
        expected = ["[", ' {"a":1}', ',{"a":2}', "]"]
        self.assertTrue(copied(copy, expected, ["[", ' {"a":2}', ',{"a":1}', "]"]))
        self.assertFalse(copied(copy, expected, [' {"a":2}', "[", ',{"a":1}', "]"]))
        self.assertFalse(copied(copy, expected, ["[", ' {"a":2}', ',{"a":3}', "]"]))
        self.assertEqual(order("copy t to stdout (format json, force_array false);"), ("copy", False, False, False))

    def test_an_ordered_query_keeps_its_order(self):
        self.assertEqual(order("copy (select * from t order by a) to stdout;"), DEFINED)
        self.assertEqual(order("copy (select * from t) to stdout;"), ("copy", False, False, False))
        self.assertEqual(order("copy t from stdin;"), DEFINED)

    def test_a_units_copied_rows_are_checked_whole(self):
        script = ["create table t (a int);", "copy t from stdin;", "1", "2", "\\.", "copy t to stdout;"]
        expected = ["create table t (a int);", "copy t from stdin;", "copy t to stdout;", "1", "2"]
        self.assertTrue(same("t", script, expected, expected[:3] + ["2", "1"]))
        self.assertFalse(same("t", script, expected, expected[:3] + ["2", "3"]))


if __name__ == "__main__":
    unittest.main()
