// Copyright 2026 Dolthub, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use harness::oid::*;
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, Dolt, E, Expected, Flow, N, PRIMARY_KEY_RETURNED, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_as_of() {
    run_scripts(&[
        ScriptTest {
            name: "Single table",
            set_up_script: &[
                "CREATE TABLE test (a INT)",
                "INSERT INTO test VALUES (1)",
                "SELECT DOLT_COMMIT('-Am', 'new table')",
                "INSERT INTO test VALUES (2)",
                "SELECT DOLT_COMMIT('-am', 'new row')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD^' as t1",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD'",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Join",
            set_up_script: &[
                "CREATE TABLE test (a INT)",
                "INSERT INTO test VALUES (1)",
                "SELECT DOLT_COMMIT('-Am', 'new table')",
                "INSERT INTO test VALUES (2)",
                "SELECT DOLT_COMMIT('-am', 'new row')",
                "CREATE TABLE test2 (b INT)",
                "INSERT INTO test2 VALUES (1)",
                "SELECT DOLT_COMMIT('-Am', 'new table')",
                "INSERT INTO test2 VALUES (2)",
                "SELECT DOLT_COMMIT('-am', 'new row')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~3' t1 join test2 AS OF 'HEAD~' t2 on t1.a = t2.b",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~3' t1 join test2 AS t2 on t1.a = t2.b",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test t1 join test2 AS OF 'HEAD~' AS t2 on t1.a = t2.b",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~3' t1 cross join test2 AS OF 'HEAD~' t2",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD' t1 cross join test2 AS OF 'HEAD~' t2",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Syntax variations",
            set_up_script: &[
                "CREATE TABLE test (a INT)",
                "INSERT INTO test VALUES (1)",
                "SELECT DOLT_COMMIT('-Am', 'new table')",
                "INSERT INTO test VALUES (2)",
                "SELECT DOLT_COMMIT('-am', 'new row')",
                "CREATE TABLE test2 (b INT)",
                "INSERT INTO test2 VALUES (1)",
                "SELECT DOLT_COMMIT('-Am', 'new table')",
                "INSERT INTO test2 VALUES (2)",
                "SELECT DOLT_COMMIT('-am', 'new row')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~3' AS t1 join test2 AS OF 'HEAD' AS t2 on t1.a = t2.b",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~3' t1 join test2 AS OF 'HEAD' t2 on t1.a = t2.b",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS t1 join test2 AS OF 'HEAD~' t2 on t1.a = t2.b",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Cockroach's AS OF SYSTEM TIME is not Dolt syntax, so Postgres' syntax error is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF SYSTEM TIME 'HEAD~3' join test2 AS t2 on test.a = t2.b",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"SYSTEM\"", position: 26, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_as_of_self_joins() {
    run_scripts(&[
        ScriptTest {
            name: "a table joined to itself as of another revision",
            set_up_script: &[
                "CREATE TABLE hist (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO hist VALUES (1, 'one'), (2, 'two');",
                "SELECT dolt_commit('-Am', 'first');",
                "UPDATE hist SET v = 'ONE' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'second');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Doltgrust's own.
                ScriptTestAssertion {
                    query: "SELECT cur.id, cur.v, old.v FROM hist cur JOIN hist AS OF 'HEAD~1' old ON cur.id = old.id ORDER BY cur.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("ONE"), T("one")],
                            &[T("2"), T("two"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_routines_in_call_as_of_and_revision_qualified_calls() {
    run_scripts(&[
        ScriptTest {
            name: "a function called inside CALL ... AS OF runs its current body over the revision's rows",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE FUNCTION f() RETURNS INT LANGUAGE sql AS 'SELECT v * 10 FROM t';",
                "CREATE PROCEDURE p(OUT r INT) LANGUAGE plpgsql AS $$ BEGIN r := f(); END $$;",
                "SELECT dolt_commit('-Am', 'first');",
                "CREATE OR REPLACE FUNCTION f() RETURNS INT LANGUAGE sql AS 'SELECT v * 100 FROM t';",
                "UPDATE t SET v = 2;",
                "SELECT dolt_commit('-am', 'second');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for CALL ... AS OF.
                ScriptTestAssertion {
                    query: "CALL p(NULL) AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a function called inside a revision-qualified CALL runs that revision's body over the revision's rows",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE FUNCTION f() RETURNS INT LANGUAGE sql AS 'SELECT v * 10 FROM t';",
                "CREATE PROCEDURE p(OUT r INT) LANGUAGE plpgsql AS $$ BEGIN r := f(); END $$;",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT dolt_branch('b');",
                "CREATE OR REPLACE FUNCTION f() RETURNS INT LANGUAGE sql AS 'SELECT v * 100 FROM t';",
                "UPDATE t SET v = 2;",
                "SELECT dolt_commit('-am', 'second');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are b's f over b's rows (Appendix C).
                ScriptTestAssertion {
                    query: "CALL \"postgres/b\".public.p(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_as_of_privileges_dependencies_and_definitions() {
    run_scripts(&[
        ScriptTest {
            name: "AS OF reads are checked as Postgres checks tables, columns matched by identity",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, a INT, b INT, gone INT)",
                "INSERT INTO t VALUES (1, 10, 100, 1000)",
                "CREATE TABLE dropped (x INT)",
                "INSERT INTO dropped VALUES (5)",
                "CREATE USER reader PASSWORD 'password'",
                "GRANT SELECT ON dropped TO reader",
                "SELECT dolt_commit('-Am', 'one')",
                "ALTER TABLE t RENAME COLUMN a TO a2",
                "ALTER TABLE t DROP COLUMN gone",
                "DROP TABLE dropped",
                "GRANT SELECT (pk, a2) ON t TO reader",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk, a FROM t AS OF 'HEAD'",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("a", INT4)], rows: &[&[T("1"), T("10")]], tag: "SELECT 1" },
                    username: "reader",
                    password: "password",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT b FROM t AS OF 'HEAD'",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table t", ..E }),
                    username: "reader",
                    password: "password",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT gone FROM t AS OF 'HEAD'",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table t", ..E }),
                    username: "reader",
                    password: "password",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dropped AS OF 'HEAD'",
                    expected: Expected::Rows { columns: &[Column("x", INT4)], rows: &[&[T("5")]], tag: "SELECT 1" },
                    username: "reader",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "views read AS OF use their current definitions, and through a revision-qualified name their definitions there, checking their relations as their owners",
            set_up_script: &[
                "CREATE ROLE vo",
                "CREATE USER reader PASSWORD 'password'",
                "CREATE USER stranger PASSWORD 'password'",
                "CREATE TABLE t2 (k INT)",
                "INSERT INTO t2 VALUES (7)",
                "ALTER TABLE t2 OWNER TO vo",
                "CREATE VIEW v2 AS SELECT k FROM t2",
                "ALTER VIEW v2 OWNER TO vo",
                "GRANT SELECT ON v2 TO reader",
                "SELECT dolt_commit('-Am', 'x')",
                "CREATE OR REPLACE VIEW v2 AS SELECT k * 100 AS k FROM t2",
                "ALTER VIEW v2 OWNER TO postgres",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM v2 AS OF 'HEAD'",
                    expected: Expected::Rows { columns: &[Column("k", INT4)], rows: &[&[T("700")]], tag: "SELECT 1" },
                    username: "reader",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table t2", ..E }),
                    username: "reader",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion { query: "DROP VIEW v2", ..A },
                ScriptTestAssertion { query: "DROP TABLE t2", ..A },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM v2 AS OF 'HEAD'",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"v2\" does not exist", ..E }),
                    username: "reader",
                    password: "password",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@HEAD\".public.v2",
                    expected: Expected::Rows { columns: &[Column("k", INT4)], rows: &[&[T("7")]], tag: "SELECT 1" },
                    username: "reader",
                    password: "password",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@HEAD\".public.v2",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for view v2", ..E }),
                    username: "stranger",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a view over an AS OF read depends on the relation and fails when the revision changes its columns",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, a INT)",
                "INSERT INTO t VALUES (1, 1)",
                "CREATE VIEW v AS SELECT pk, a FROM t",
                "SELECT dolt_commit('-Am', 'one')",
                "CREATE VIEW w AS SELECT * FROM v AS OF 'HEAD'",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "DROP VIEW v",
                    expected: Expected::Error(Diagnostic {
                        code: "2BP01",
                        message: "cannot drop view v because other objects depend on it",
                        detail: "view w depends on view v",
                        hint: "Use DROP ... CASCADE to drop the dependent objects too.",
                        ..E
                    }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('w'::regclass)",
                    expected: Expected::Rows { columns: &[Column("pg_get_viewdef", TEXT)], rows: &[&[T(" SELECT pk,\n    a\n   FROM v AS OF 'HEAD';")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "CREATE OR REPLACE VIEW v AS SELECT pk, a, a::text AS b FROM t", ..A },
                ScriptTestAssertion { query: "SELECT dolt_commit('-Am', 'two')", ..A },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM w",
                    expected: Expected::Error(Diagnostic {
                        code: "42804",
                        message: "function return row and query-specified return row do not match",
                        detail: "Returned row contains 3 attributes, but query expects 2.",
                        ..E
                    }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_as_of_rows is called only through AS OF",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY)",
                "SELECT dolt_commit('-Am', 'one')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_as_of_rows('public', 't', 't'::regclass, 'HEAD', NULL::int2[], 0::oid)",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "dolt_as_of_rows can only be called through AS OF", position: 15, ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CREATE VIEW sneaky AS SELECT * FROM dolt_as_of_rows('public', 't', 't'::regclass, 'HEAD', NULL::int2[], 0::oid)",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "dolt_as_of_rows can only be called through AS OF", position: 37, ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'SELECT * FROM dolt_as_of_rows(''public'', ''t'', ''t''::regclass, ''HEAD'', NULL::int2[], 0::oid)'; END $$",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "dolt_as_of_rows can only be called through AS OF", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE p AS SELECT * FROM dolt_as_of_rows('public', 't', 't'::regclass, 'HEAD', NULL::int2[], 0::oid)",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "dolt_as_of_rows can only be called through AS OF", position: 28, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "views with AS OF reads survive pg_get_viewdef",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, a INT, b TEXT)",
                "INSERT INTO t VALUES (1, 10, 'x'), (2, 20, 'y')",
                "SELECT dolt_commit('-Am', 'one')",
                "UPDATE t SET a = a + 1",
                "SELECT dolt_commit('-Am', 'two')",
                "CREATE VIEW v1 AS SELECT * FROM t AS OF 'HEAD~1'",
                "CREATE VIEW v2 AS SELECT o.pk, o.aa AS old_a, c.a AS new_a FROM t AS OF 'HEAD~1' AS o(pk, aa) JOIN t c USING (pk) WHERE o.aa > 0",
                "CREATE VIEW v3 AS SELECT x.* FROM public.t AS OF dolt_hashof('HEAD~1') x, t AS OF main y WHERE x.pk = y.pk",
                "DO $$ BEGIN EXECUTE 'CREATE VIEW v1r AS ' || pg_get_viewdef('v1'::regclass); EXECUTE 'CREATE VIEW v2r AS ' || pg_get_viewdef('v2'::regclass); EXECUTE 'CREATE VIEW v3r AS ' || pg_get_viewdef('v3'::regclass); END $$",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT array_agg(v ORDER BY v) FROM v1 v)::text = (SELECT array_agg(v ORDER BY v) FROM v1r v)::text, (SELECT array_agg(v ORDER BY v) FROM v2 v)::text = (SELECT array_agg(v ORDER BY v) FROM v2r v)::text, (SELECT array_agg(v ORDER BY v) FROM v3 v)::text = (SELECT array_agg(v ORDER BY v) FROM v3r v)::text",
                    expected: Expected::Rows { columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)], rows: &[&[T("t"), T("t"), T("t")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL ... AS OF runs the current procedures over the relations of the revision, and writes nothing",
            set_up_script: &[
                "CREATE TABLE test (v1 BIGINT)",
                "INSERT INTO test VALUES (1)",
                "CREATE PROCEDURE p1(OUT v1 BIGINT) LANGUAGE plpgsql AS $$ BEGIN CALL p2(v1); END $$",
                "CREATE PROCEDURE p2(OUT v1 BIGINT) LANGUAGE plpgsql AS $$ BEGIN SELECT test.v1 INTO v1 FROM test; END $$",
                "SELECT dolt_commit('-Am', 'one')",
                "UPDATE test SET v1 = 2",
                "CREATE OR REPLACE PROCEDURE p2(OUT v1 BIGINT) LANGUAGE plpgsql AS $$ BEGIN SELECT test.v1 * 100 INTO v1 FROM test; END $$",
                "SELECT dolt_commit('-Am', 'two')",
                "UPDATE test SET v1 = 3",
                "SELECT dolt_commit('-Am', 'three')",
                "CREATE PROCEDURE w() LANGUAGE plpgsql AS $$ BEGIN INSERT INTO test VALUES (9); END $$",
                "CREATE PROCEDURE nested(OUT v BIGINT) LANGUAGE plpgsql AS $$ BEGIN CALL p2(v) AS OF 'HEAD~2'; END $$",
                "CREATE SEQUENCE s",
                "CREATE PROCEDURE n() LANGUAGE plpgsql AS $$ BEGIN PERFORM nextval('s'); END $$",
                "CREATE PROCEDURE c() LANGUAGE plpgsql AS $$ BEGIN PERFORM dolt_commit('--allow-empty', '-m', 'x'); END $$",
                "SELECT dolt_commit('-Am', 'four')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD~2'",
                    expected: Expected::Rows { columns: &[Column("v1", INT8)], rows: &[&[T("200")]], tag: "CALL" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD~3'",
                    expected: Expected::Rows { columns: &[Column("v1", INT8)], rows: &[&[T("100")]], tag: "CALL" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL nested(NULL)",
                    expected: Expected::Rows { columns: &[Column("v", INT8)], rows: &[&[T("200")]], tag: "CALL" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL w() AS OF 'HEAD'",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "CALL ... AS OF converts databases to read only", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL n() AS OF 'HEAD'",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute nextval() in a read-only transaction", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL c() AS OF 'HEAD'",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute dolt_commit() in a read-only transaction", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('s')",
                    expected: Expected::Rows { columns: &[Column("nextval", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_revision_qualified_relations() {
    run_scripts(&[
        ScriptTest {
            name: "a revision-qualified relation reads the revision's rows, and a view made over one deparses its qualifier",
            set_up_script: &[
                "CREATE TABLE q (k INT PRIMARY KEY, v TEXT)",
                "INSERT INTO q VALUES (1, 'a')",
                "SELECT dolt_commit('-Am', 'one')",
                "UPDATE q SET v = 'b'",
                "SELECT dolt_commit('-am', 'two')",
                "SELECT dolt_branch('other')",
                "UPDATE q SET v = 'c'",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@HEAD~1\".public.q",
                    expected: Expected::Rows { columns: &[Column("k", INT4), Column("v", TEXT)], rows: &[&[T("1"), T("a")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM \"postgres/HEAD\".public.q",
                    expected: Expected::Rows { columns: &[Column("v", TEXT)], rows: &[&[T("b")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM \"postgres/other\".public.q",
                    expected: Expected::Rows { columns: &[Column("v", TEXT)], rows: &[&[T("b")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM \"otherdb@HEAD\".public.q",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cross-database references are not implemented: \"otherdb@HEAD.public.q\"", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "CREATE VIEW hv AS SELECT * FROM \"postgres@HEAD~1\".public.q", ..A },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('hv')",
                    expected: Expected::Rows { columns: &[Column("pg_get_viewdef", TEXT)], rows: &[&[T(" SELECT k,\n    v\n   FROM \"postgres/HEAD~1\".public.q;")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM hv",
                    expected: Expected::Rows { columns: &[Column("k", INT4), Column("v", TEXT)], rows: &[&[T("1"), T("a")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "CREATE VIEW tv AS SELECT k FROM q", ..A },
                ScriptTestAssertion { query: "SELECT dolt_commit('-Am', 'three')", ..A },
                ScriptTestAssertion { query: "DROP VIEW tv", ..A },
                ScriptTestAssertion { query: "CREATE TABLE tv (k INT)", ..A },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM tv AS OF 'HEAD'",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"tv\" does not exist", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@HEAD\".public.tv",
                    expected: Expected::Rows { columns: &[Column("k", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_system_views_as_of() {
    run_scripts(&[
        ScriptTest {
            name: "Dolt's system views read AS OF a revision read that revision in place of the session's head",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY)",
                "SELECT dolt_commit('-Am', 'one')",
                "INSERT INTO t VALUES (1)",
                "SELECT dolt_commit('-am', 'two')",
                "INSERT INTO t VALUES (2)",
                "SELECT dolt_branch('b')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log AS OF 'HEAD~1' ORDER BY commit_order DESC",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[&[T("one")], &[T("Initialize data repository")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'HEAD~1'",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'HEAD'",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[&[T("public.t"), T("f"), T("modified")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, data_change, schema_change FROM dolt_diff AS OF 'HEAD~1'",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[&[T("public.t"), T("f"), T("t")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'b'",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(ARRAY['x'], 'y')",
                    expected: Expected::Error(Diagnostic {
                        code: "42883",
                        message: "function dolt_log(text[], unknown) does not exist",
                        hint: "No function matches the given name and argument types. You might need to add explicit type casts.",
                        position: 15,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion { query: "CREATE VIEW lv AS SELECT message FROM dolt_log AS OF 'HEAD~1'", ..A },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('lv')",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[&[T(" SELECT message\n   FROM dolt_log AS OF 'HEAD~1';")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM lv",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_call_as_of_runs_a_procedure_replaced_since_its_last_call() {
    run_scripts(&[
        ScriptTest {
            name: "CALL ... AS OF runs a PL/pgSQL procedure's new body after CREATE OR REPLACE in the same session",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE PROCEDURE p(OUT r INT) LANGUAGE plpgsql AS $$ BEGIN SELECT v * 10 INTO r FROM t; END $$;",
                "SELECT dolt_commit('-Am', 'first');",
                "UPDATE t SET v = 2;",
                "SELECT dolt_commit('-am', 'second');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for CALL ... AS OF.
                ScriptTestAssertion {
                    query: "CALL p(NULL) AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE PROCEDURE p(OUT r INT) LANGUAGE plpgsql AS $$ BEGIN SELECT v * 100 INTO r FROM t; END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for CALL ... AS OF.
                ScriptTestAssertion {
                    query: "CALL p(NULL) AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_as_of_a_time() {
    run_scripts(&[
        ScriptTest {
            name: "AS OF a time-typed value reads the newest commit at or before it on the branch's first-parent history",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'first', '--date', '2020-03-01T12:00:00Z');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'second', '--date', '2020-03-01T13:00:00Z');",
                "SET TimeZone = 'UTC';",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for AS OF a time.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t AS OF TIMESTAMP '2020-03-01 12:00:00';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for AS OF a time.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t AS OF '2020-03-01 12:59:59+00'::timestamptz;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for AS OF a time.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t AS OF 'infinity'::timestamptz;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for AS OF a time.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t AS OF CAST(TIMESTAMPTZ '2020-03-01 13:30:00+00' - interval '1 minute' AS timestamptz);",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TimeZone = 'America/New_York';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for AS OF a time.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t AS OF TIMESTAMP '2020-03-01 07:30:00';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for AS OF a time.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t AS OF TIMESTAMP '2020-03-01 06:59:59';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t\" does not exist", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for AS OF a time.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t AS OF '2020-03-01 12:00:00';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: 2020-03-01 12:00:00", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for AS OF a time.
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log AS OF TIMESTAMP '2020-03-01 07:30:00' LIMIT 1;",
                    expected: Expected::Rows { columns: &[Column("message", TEXT)], rows: &[&[T("first")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log AS OF TIMESTAMP '2020-03-01 06:59:59';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "dolt_log AS OF a time before every commit of the branch reads no commit", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE p(OUT r bigint) LANGUAGE plpgsql AS $$ BEGIN SELECT count(*) INTO r FROM t; END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt's rule for CALL ... AS OF.
                ScriptTestAssertion {
                    query: "CALL p(NULL) AS OF TIMESTAMP '2020-03-01 07:30:00';",
                    expected: Expected::Rows { columns: &[Column("r", INT8)], rows: &[&[T("1")]], tag: "CALL" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL p(NULL) AS OF TIMESTAMP '2020-03-01 06:59:59';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "CALL AS OF a time before every commit of the branch reads no commit", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "AS OF a time reads the first commit at or before it in dolt_log's order, a merge's second parent included",
            set_up_script: &[
                "SET TimeZone = 'UTC';",
                "CREATE TABLE t (v INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'c1', '--date', '2020-03-01T12:00:00Z');",
                "SELECT dolt_checkout('-b', 'b');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'b1', '--date', '2020-03-01T13:30:00Z');",
                "SELECT dolt_checkout('main');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'm1', '--date', '2020-03-01T13:00:00Z');",
                "SELECT dolt_merge('b', '-m', 'merge');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt main's (614771d742) for
                // the same history, which reads b1, the merge's second parent, at 13:45.
                ScriptTestAssertion {
                    query: "SELECT v FROM t AS OF TIMESTAMP '2020-03-01 13:45:00' ORDER BY v;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")], &[T("3")]], tag: "SELECT 2" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are Dolt main's (614771d742) for
                // the same history.
                ScriptTestAssertion {
                    query: "SELECT v FROM t AS OF TIMESTAMP '2020-03-01 13:15:00' ORDER BY v;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_per_table_system_views_as_of() {
    run_scripts(&[
        ScriptTest {
            name: "per-table system views read AS OF a revision read that revision in place of the session's head",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY)",
                "SELECT dolt_commit('-Am', 'one')",
                "SELECT set_config('test.one', dolt_hashof('HEAD')::text, false)",
                "INSERT INTO t VALUES (1)",
                "SELECT dolt_commit('-am', 'two')",
                "INSERT INTO t VALUES (2)",
                "SELECT dolt_commit('-am', 'three')",
                "INSERT INTO t VALUES (3)",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT k FROM dolt_history_t AS OF 'HEAD~1' ORDER BY k",
                    expected: Expected::Rows { columns: &[Column("k", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT to_k, dolt_diff_type FROM dolt_diff_t AS OF 'HEAD~1' ORDER BY to_k",
                    expected: Expected::Rows {
                        columns: &[Column("to_k", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[&[T("1"), T("added")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT to_k, dolt_diff_type FROM dolt_commit_diff_t AS OF 'HEAD~1' WHERE dolt_to_commit = 'WORKING' AND dolt_from_commit = current_setting('test.one') ORDER BY to_k",
                    expected: Expected::Rows {
                        columns: &[Column("to_k", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[&[T("1"), T("added")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT to_k FROM dolt_commit_diff_t WHERE dolt_to_commit = 'WORKING' AND dolt_from_commit = current_setting('test.one') ORDER BY to_k",
                    expected: Expected::Rows {
                        columns: &[Column("to_k", INT4)],
                        rows: &[&[T("1")], &[T("2")], &[T("3")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_as_of_relations_planned_as_relations() {
    run_scripts(&[
        ScriptTest {
            name: "a relation read AS OF a revision plans with the revision's indexes",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, v TEXT);",
                "CREATE INDEX t_v ON t (v);",
                "INSERT INTO t SELECT i, 'v' || i FROM generate_series(1, 1000) i;",
                "SELECT dolt_commit('-Am', 'one');",
                "DROP INDEX t_v;",
                "INSERT INTO t VALUES (2000, 'new');",
                "SELECT dolt_commit('-Am', 'two');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t AS OF 'HEAD~1' WHERE v = 'v5';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on t")],
                            &[T("  Recheck Cond: (v = 'v5'::text)")],
                            &[T("  ->  Bitmap Index Scan on t_v")],
                            &[T("        Index Cond: (v = 'v5'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan using t_v on t", "  Index Cond: (v = 'v5'::text)"])),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk FROM t AS OF 'HEAD~1' WHERE v = 'v5';",
                    expected: Expected::Rows { columns: &[Column("pk", INT4)], rows: &[&[T("5")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t AS OF 'HEAD~1' x JOIN t y ON x.pk = y.pk;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1000")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a prepared statement AS OF a branch reads the branch's newest commit, and AS OF a tag its tag's",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_tag('v1');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE m AS SELECT count(*) FROM t AS OF 'main';",
                    expected: Expected::Tag("PREPARE"),
                    client: "other",
                    database: "postgres/other",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE v AS SELECT count(*) FROM t AS OF 'v1';",
                    expected: Expected::Tag("PREPARE"),
                    client: "other",
                    database: "postgres/other",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE m;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "other",
                    database: "postgres/other",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit('-am', 'two');",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE m;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    client: "other",
                    database: "postgres/other",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE v;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "other",
                    database: "postgres/other",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_stored_as_of_reads_planned_as_relations() {
    run_scripts(&[
        ScriptTest {
            name: "stored AS OF reads in views, a view over a view, rules, SQL-standard bodies, materialized views, and cursors read the revision as a relation",
            set_up_script: &["CREATE TABLE t (pk INT PRIMARY KEY, a INT, b TEXT);", "INSERT INTO t SELECT g, g * 10, 'x' || g FROM generate_series(1, 5) g;", "SELECT dolt_commit('-Am', 'one');", "ALTER TABLE t DROP COLUMN a;", "ALTER TABLE t ADD COLUMN a INT;", "UPDATE t SET a = 0;", "SELECT dolt_commit('-Am', 'two');", "CREATE VIEW v1 AS SELECT pk, a, b FROM t AS OF 'HEAD~1' WHERE pk < 3;", "CREATE VIEW vv AS SELECT * FROM v1 WHERE pk = 2;", "CREATE FUNCTION old_a(k INT) RETURNS INT BEGIN ATOMIC SELECT a FROM t AS OF 'HEAD~1' WHERE pk = k; END;", "CREATE TABLE src (pk INT);", "CREATE TABLE log (pk INT, old INT);", "CREATE RULE r AS ON INSERT TO src DO ALSO INSERT INTO log SELECT new.pk, a FROM t AS OF 'HEAD~1' WHERE t.pk = new.pk;", "CREATE MATERIALIZED VIEW m1 AS SELECT * FROM t AS OF 'HEAD~1' WHERE pk > 3;", "SET enable_seqscan = off;"],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("x1")],
                            &[T("2"), T("20"), T("x2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM v1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using t_pkey on t")],
                            &[T("  Index Cond: (pk < 3)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM vv;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("20"), T("x2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM vv;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using t_pkey on t")],
                            &[T("  Index Cond: ((pk < 3) AND (pk = 2))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT old_a(3);",
                    expected: Expected::Rows {
                        columns: &[Column("old_a", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO src VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM log;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("old", INT4)],
                        rows: &[
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO src VALUES (5);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Insert on src")],
                            &[T("  ->  Result")],
                            &[T("")],
                            &[T("Insert on log")],
                            &[T("  ->  Index Scan using t_pkey on t")],
                            &[T("        Index Cond: (pk = 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM m1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("4"), T("40"), T("x4")],
                            &[T("5"), T("50"), T("x5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW m1;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM m1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CREATE TABLE c AS SELECT * FROM v1;",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM c ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("x1")],
                            &[T("2"), T("20"), T("x2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW m2 AS SELECT * FROM v1;",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW m2;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM m2;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT * FROM v1 ORDER BY pk;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("x1")],
                            &[T("2"), T("20"), T("x2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a stored AS OF read keeps its relation's dependency and fails once its revision lacks a column it reads",
            set_up_script: &["CREATE TABLE t (pk INT PRIMARY KEY, a INT, b TEXT);", "INSERT INTO t VALUES (1, 10, 'x');", "SELECT dolt_commit('-Am', 'one');", "SELECT dolt_branch('other');", "CREATE VIEW w AS SELECT pk, b FROM t AS OF 'other';"],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table t because other objects depend on it", detail: "view w depends on table t", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP COLUMN b;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'drop b');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM w;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"b\" of relation \"t\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a revision named by an expression reads as a relation and is evaluated again for each run of a prepared statement",
            set_up_script: &["CREATE TABLE e (pk INT PRIMARY KEY, v INT);", "INSERT INTO e VALUES (1, 1);", "SELECT dolt_commit('-Am', 'one');", "UPDATE e SET v = 2;", "SELECT dolt_commit('-Am', 'two');", "CREATE VIEW ev AS SELECT v FROM e AS OF dolt_hashof('HEAD~1');"],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT v FROM e AS OF dolt_hashof('HEAD~1');",
                    expected: Expected::Rows { columns: &[Column("QUERY PLAN", TEXT)], rows: &[&[T("Seq Scan on e")]], tag: "EXPLAIN" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM ev;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT v FROM ev;",
                    expected: Expected::Rows { columns: &[Column("QUERY PLAN", TEXT)], rows: &[&[T("Seq Scan on e")]], tag: "EXPLAIN" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE p AS SELECT v FROM e AS OF dolt_hashof('HEAD~1');",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE p;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "UPDATE e SET v = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'three');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE p;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM ev;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "row locks on a relation read AS OF a revision lock nothing there and read the revision's rows",
            set_up_script: &["CREATE TABLE e (pk INT PRIMARY KEY, v INT);", "INSERT INTO e VALUES (1, 1);", "SELECT dolt_commit('-Am', 'one');", "UPDATE e SET v = 2;", "SELECT dolt_commit('-Am', 'two');"],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM e AS OF 'HEAD~1' FOR UPDATE;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT x.v, y.v FROM e AS OF 'HEAD~1' x JOIN e y USING (pk) FOR SHARE;",
                    expected: Expected::Rows { columns: &[Column("v", INT4), Column("v", INT4)], rows: &[&[T("1"), T("2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CREATE VIEW ev AS SELECT * FROM e AS OF 'HEAD~1';",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM ev FOR UPDATE;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT * FROM e AS OF 'HEAD~1' x FOR UPDATE OF x;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "FETCH c;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")]], tag: "FETCH 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "UPDATE e SET v = 100 WHERE CURRENT OF c;",
                    expected: Expected::Error(Diagnostic { code: "24000", message: "cursor \"c\" does not have a FOR UPDATE/SHARE reference to table \"e\"", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM e AS OF 'br' FOR UPDATE;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("1"), T("2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SET lock_timeout = '1s';",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "UPDATE e SET v = 5;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM e;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("1"), T("5")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a revision named by a parameter takes the value each run binds, through PREPARE and the extended protocol",
            set_up_script: &["CREATE TABLE f (pk INT PRIMARY KEY, v INT);", "INSERT INTO f VALUES (1, 1);", "SELECT dolt_commit('-Am', 'one');", "ALTER TABLE f ADD COLUMN w INT DEFAULT 7;", "UPDATE f SET v = 2;", "SELECT dolt_commit('-Am', 'two');"],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE q(text) AS SELECT v FROM f AS OF $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE q('HEAD~1');",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE q('HEAD');",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE q('HEAD~1');",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM f AS OF $1;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    bind_vars: &[BindVar::Str("HEAD~1")],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM f AS OF $1;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    bind_vars: &[BindVar::Str("HEAD")],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE r(text) AS SELECT * FROM f AS OF $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE r('HEAD');",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4), Column("w", INT4)], rows: &[&[T("1"), T("2"), T("7")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE r('HEAD~1');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cached plan must not change result type", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "functions that read AS OF, called from a statement that reads AS OF, read their own revisions",
            set_up_script: &["CREATE TABLE u (pk INT PRIMARY KEY, a INT);", "INSERT INTO u SELECT g, g FROM generate_series(1, 3) g;", "SELECT dolt_commit('-Am', 'one');", "UPDATE u SET a = a * 10;", "SELECT dolt_commit('-Am', 'two');", "UPDATE u SET a = a * 10;", "CREATE FUNCTION pl_a(k INT) RETURNS INT LANGUAGE plpgsql AS $$ DECLARE x INT; y INT; BEGIN SELECT a INTO x FROM u WHERE pk = k; SELECT a INTO y FROM u AS OF 'HEAD' WHERE pk = k; RETURN x + y; END $$;", "CREATE FUNCTION sql_a(k INT) RETURNS INT LANGUAGE sql VOLATILE AS $$ SELECT 0; SELECT a FROM u AS OF 'HEAD' WHERE pk = k $$;", "CREATE FUNCTION pl_sum() RETURNS INT LANGUAGE plpgsql AS $$ DECLARE s INT := 0; r RECORD; BEGIN FOR r IN SELECT a FROM u AS OF 'HEAD~1' LOOP s := s + r.a + sql_a(1); END LOOP; RETURN s; END $$;"],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk, a, pl_a(pk), sql_a(pk) FROM u AS OF 'HEAD~1' ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("a", INT4), Column("pl_a", INT4), Column("sql_a", INT4)], rows: &[&[T("1"), T("1"), T("110"), T("10")], &[T("2"), T("2"), T("220"), T("20")], &[T("3"), T("3"), T("330"), T("30")]], tag: "SELECT 3" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk, a, pl_a(pk), sql_a(pk) FROM u AS OF 'HEAD~1' ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("a", INT4), Column("pl_a", INT4), Column("sql_a", INT4)], rows: &[&[T("1"), T("1"), T("110"), T("10")], &[T("2"), T("2"), T("220"), T("20")], &[T("3"), T("3"), T("330"), T("30")]], tag: "SELECT 3" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk, a, pl_sum() FROM u AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("a", INT4), Column("pl_sum", INT4)], rows: &[&[T("1"), T("10"), T("36")], &[T("2"), T("20"), T("36")], &[T("3"), T("30"), T("36")]], tag: "SELECT 3" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk, a, pl_sum() FROM u AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("a", INT4), Column("pl_sum", INT4)], rows: &[&[T("1"), T("10"), T("36")], &[T("2"), T("20"), T("36")], &[T("3"), T("30"), T("36")]], tag: "SELECT 3" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_as_of_reads_over_a_newer_catalog() {
    run_scripts(&[
        ScriptTest {
            name: "a relation read AS OF a revision keeps its copy while a detach of a partition is pending, whether or not the snapshot sees it",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt SELECT i, 'x' FROM generate_series(0, 19) i;",
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'one');",
                "INSERT INTO t VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("20")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET lock_timeout = '200ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 CONCURRENTLY;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ RESET lock_timeout;",
                    expected: Expected::Tag("RESET"),
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM t AS OF 'HEAD';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM t AS OF 'HEAD';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 FINALIZE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a cached plan that reads AS OF and waited for a lock on a current inheritance child plans over the newer catalog and replans after another change",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'one');",
                "INSERT INTO t VALUES (2);",
                "CREATE TABLE p (a INT);",
                "CREATE TABLE c () INHERITS (p);",
                "INSERT INTO c VALUES (5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET plan_cache_mode = force_generic_plan;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE q AS SELECT (SELECT count(*) FROM t AS OF 'HEAD') AS head, (SELECT count(*) FROM p) AS current;",
                    expected: Expected::Tag("PREPARE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD COLUMN x INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows { columns: &[Column("head", INT8), Column("current", INT8)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows { columns: &[Column("head", INT8), Column("current", INT8)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD COLUMN y INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES (6, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows { columns: &[Column("head", INT8), Column("current", INT8)], rows: &[&[T("1"), T("2")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT generic_plans > 0 FROM pg_prepared_statements WHERE name = 'q';",
                    expected: Expected::Rows { columns: &[Column("?column?", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a statement that reads AS OF after its session dropped a current table's primary key reads the table without it",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'one');",
                "CREATE TABLE p (a INT PRIMARY KEY, b INT);",
                "INSERT INTO p VALUES (1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE p DROP CONSTRAINT p_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (1, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM t AS OF 'HEAD') AS head, (SELECT count(*) FROM p WHERE a = 1) AS current;",
                    expected: Expected::Rows { columns: &[Column("head", INT8), Column("current", INT8)], rows: &[&[T("1"), T("2")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_branch_qualified_names_read_the_branchs_working_state() {
    run_scripts(&[
        ScriptTest {
            name: "a branch-qualified name reads the branch's working state, its own branch's the session's relation, and AS OF the branch its head",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, v INT);",
                "CREATE INDEX t_v ON t (v);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (5, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN w TEXT DEFAULT 'b1';",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4), Column("w", TEXT)], rows: &[&[T("1"), T("1"), T("b1")], &[T("5"), T("5"), T("b1")]], tag: "SELECT 2" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'b1' ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk FROM \"postgres@b1\".public.t WHERE v = 5;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4)], rows: &[&[T("5")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE q AS SELECT count(*) FROM \"postgres/b1\".public.t;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (6, 6, 'x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN z INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4), Column("w", TEXT), Column("z", INT4)], rows: &[&[T("1"), T("1"), T("b1"), Null], &[T("5"), T("5"), T("b1"), Null], &[T("6"), T("6"), T("x"), Null]], tag: "SELECT 3" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a write through a revision-qualified name of a tag or commit fails as in a read-only transaction, and one of the session's own branch writes it",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_tag('v1');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/v1\".public.t VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    dolt: Some(Dolt::Differs("Dolt refuses a write to a revision database of a tag or commit with \"Database db/v1 is read-only.\", where Doltgrust fails it as Postgres fails a write in a read-only transaction")),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/HEAD\".public.t SET v = 3;",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute UPDATE in a read-only transaction", ..E }),
                    dolt: Some(Dolt::Differs("Dolt refuses a write to a revision database of a tag or commit with \"Database db/v1 is read-only.\", where Doltgrust fails it as Postgres fails a write in a read-only transaction")),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4)], rows: &[&[T("1")], &[T("3")]], tag: "SELECT 2" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch-qualified read that waited for a lock on a partition the branch changed reads the branch's newer catalog",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt VALUES (1, 'x');",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt2 ALTER COLUMN b SET DEFAULT 'z';",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt2 (a) VALUES (15);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    database: "postgres/b1",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT a, b FROM \"postgres/b1\".public.pt ORDER BY a;",
                    expected: Expected::Rows { columns: &[Column("a", INT4), Column("b", TEXT)], rows: &[&[T("1"), T("x")], &[T("15"), T("z")]], tag: "SELECT 2" },
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    database: "postgres/b1",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT a, b FROM \"postgres/b1\".public.pt ORDER BY a;",
                    expected: Expected::Rows { columns: &[Column("a", INT4), Column("b", TEXT)], rows: &[&[T("1"), T("x")], &[T("15"), T("z")]], tag: "SELECT 2" },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a relation read AS OF a revision or through a branch-qualified name reads its inheritance children's rows",
            set_up_script: &[
                "CREATE TABLE p (a INT);",
                "CREATE TABLE c () INHERITS (p);",
                "INSERT INTO p VALUES (1);",
                "INSERT INTO c VALUES (2);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT a FROM p AS OF 'HEAD' ORDER BY a;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT a FROM \"postgres/b1\".public.p ORDER BY a;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "after a procedure checks out another branch, a name qualified by the branch it left reads that branch",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "CREATE PROCEDURE p(INOUT left_branch BIGINT, INOUT this_branch BIGINT) LANGUAGE plpgsql AS $$ BEGIN PERFORM dolt_checkout('b1'); SELECT count(*) INTO left_branch FROM \"postgres/main\".public.t; SELECT count(*) INTO this_branch FROM t; END $$;",
                "CREATE PROCEDURE q() LANGUAGE plpgsql AS $$ BEGIN PERFORM dolt_checkout('b1'); TRUNCATE \"postgres/main\".public.t; END $$;",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2), (3);",
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL p(NULL, NULL);",
                    expected: Expected::Rows { columns: &[Column("left_branch", INT8), Column("this_branch", INT8)], rows: &[&[T("1"), T("3")]], tag: "CALL" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "CALL q();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cross-database references are not implemented: \"postgres/main.public.t\"", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM \"postgres/main\".public.t) AS main, (SELECT count(*) FROM \"postgres/b1\".public.t) AS b1;",
                    expected: Expected::Rows { columns: &[Column("main", INT8), Column("b1", INT8)], rows: &[&[T("1"), T("3")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a partition or inheritance child read alone AS OF a revision or through a branch-qualified name stays out of the current parent's scan",
            set_up_script: &[
                "CREATE TABLE pt (a INT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20) PARTITION BY RANGE (a);",
                "CREATE TABLE pt2a PARTITION OF pt2 FOR VALUES FROM (10) TO (15);",
                "CREATE INDEX ON pt (a);",
                "CREATE TABLE p (a INT);",
                "CREATE TABLE c () INHERITS (p);",
                "INSERT INTO pt VALUES (1), (12);",
                "INSERT INTO p VALUES (1);",
                "INSERT INTO c VALUES (2);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO pt VALUES (5), (13);",
                "INSERT INTO c VALUES (3);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM pt) AS main_pt, (SELECT count(*) FROM pt1 AS OF 'HEAD') AS head_pt1;",
                    expected: Expected::Rows { columns: &[Column("main_pt", INT8), Column("head_pt1", INT8)], rows: &[&[T("4"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM pt) AS main_pt, (SELECT count(*) FROM \"postgres/b1\".public.pt1) AS b1_pt1;",
                    expected: Expected::Rows { columns: &[Column("main_pt", INT8), Column("b1_pt1", INT8)], rows: &[&[T("4"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM pt) AS main_pt, (SELECT count(*) FROM pt2 AS OF 'HEAD' WHERE a > 11) AS head_pt2;",
                    expected: Expected::Rows { columns: &[Column("main_pt", INT8), Column("head_pt2", INT8)], rows: &[&[T("4"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM p) AS main_p, (SELECT count(*) FROM c AS OF 'HEAD') AS head_c;",
                    expected: Expected::Rows { columns: &[Column("main_p", INT8), Column("head_c", INT8)], rows: &[&[T("3"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM p) AS main_p, (SELECT count(*) FROM \"postgres/b1\".public.c) AS b1_c;",
                    expected: Expected::Rows { columns: &[Column("main_p", INT8), Column("b1_c", INT8)], rows: &[&[T("3"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a view of a branch read through its revision-qualified name calls the branch's functions",
            set_up_script: &[
                "CREATE FUNCTION f() RETURNS TEXT LANGUAGE plpgsql AS $$ BEGIN RETURN 'main'; END $$;",
                "CREATE VIEW v AS SELECT f() AS x;",
                "CREATE AGGREGATE mysum(INT) (SFUNC = int4pl, STYPE = INT, INITCOND = '0');",
                "CREATE TABLE t (a INT);",
                "INSERT INTO t VALUES (1), (2);",
                "CREATE VIEW va AS SELECT mysum(a) AS s FROM t;",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_checkout('-b', 'b1');",
                "CREATE OR REPLACE FUNCTION f() RETURNS TEXT LANGUAGE plpgsql AS $$ BEGIN RETURN 'b1'; END $$;",
                "CREATE FUNCTION h(n INT DEFAULT 5) RETURNS TEXT LANGUAGE sql AS $$ SELECT 'only b1 ' || n $$;",
                "CREATE VIEW w AS SELECT h() AS y, h(7) AS y7;",
                "CREATE FUNCTION same(x ANYELEMENT) RETURNS ANYELEMENT LANGUAGE sql AS $$ SELECT x $$;",
                "CREATE FUNCTION pick(VARIADIC xs INT[]) RETURNS INT LANGUAGE sql AS $$ SELECT xs[1] + 0 $$;",
                "CREATE VIEW pv AS SELECT same(41) + 1 AS a, same('t'::TEXT) || 'x' AS b, pick(5, 6) AS c;",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT x FROM v) AS main_v, (SELECT x FROM \"postgres/b1\".public.v) AS b1_v;",
                    expected: Expected::Rows { columns: &[Column("main_v", TEXT), Column("b1_v", TEXT)], rows: &[&[T("main"), T("b1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.w;",
                    expected: Expected::Rows { columns: &[Column("y", TEXT), Column("y7", TEXT)], rows: &[&[T("only b1 5"), T("only b1 7")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.va;",
                    expected: Expected::Rows { columns: &[Column("s", INT4)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    skip: Some("revision_design §13: a branch's aggregates called by its view read through a revision-qualified name are not copied yet"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.pv;",
                    expected: Expected::Rows { columns: &[Column("a", INT4), Column("b", TEXT), Column("c", INT4)], rows: &[&[T("42"), T("tx"), T("5")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "writes through a branch-qualified name change the branch's working state, with its sequences, and its cached plans follow the branch's definitions",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, v INT);",
                "CREATE SEQUENCE sq;",
                "CREATE TABLE s (id INT PRIMARY KEY DEFAULT nextval('sq'), v TEXT);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_checkout('-b', 'b1');",
                "ALTER SEQUENCE sq RESTART WITH 100;",
                "SELECT dolt_commit('-am', 'restart');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 2) RETURNING pk, v;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("2"), T("2")]], tag: "INSERT 0 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 5) ON CONFLICT (pk) DO UPDATE SET v = excluded.v RETURNING pk, v;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("2"), T("5")]], tag: "INSERT 0 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "MERGE INTO \"postgres/b1\".public.t AS d USING (VALUES (2, 7), (3, 3)) AS src(pk, v) ON d.pk = src.pk WHEN MATCHED THEN UPDATE SET v = src.v WHEN NOT MATCHED THEN INSERT VALUES (src.pk, src.v);",
                    expected: Expected::Tag("MERGE 2"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk, v FROM \"postgres/b1\".public.t ORDER BY pk FOR UPDATE;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")], &[T("2"), T("7")], &[T("3"), T("3")]], tag: "SELECT 3" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/b1\".public.t WHERE pk = 3 RETURNING pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4)], rows: &[&[T("3")]], tag: "DELETE 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pk, v FROM t ORDER BY pk;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.s (v) VALUES ('b1') RETURNING id;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("100")]], tag: "INSERT 0 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT currval('sq');",
                    expected: Expected::Rows { columns: &[Column("currval", INT8)], rows: &[&[T("100")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO s (v) VALUES ('main') RETURNING id;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")]], tag: "INSERT 0 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM \"postgres/b1\".pg_catalog.dolt_status ORDER BY table_name;",
                    expected: Expected::Rows { columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)], rows: &[&[T("public.s"), T("f"), T("modified")], &[T("public.t"), T("f"), T("modified")], &[T("sequence public.sq"), T("f"), T("modified")]], tag: "SELECT 3" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "PREPARE ins(INT) AS INSERT INTO \"postgres/b1\".public.t VALUES ($1, 0) RETURNING *;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE ins(10);",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("v", INT4)], rows: &[&[T("10"), T("0")]], tag: "INSERT 0 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN w INT DEFAULT 9;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "EXECUTE ins(11);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cached plan must not change result type", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SET default_transaction_read_only = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (12, 0);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch-qualified write takes the branch's defaults, CHECK constraints, index expressions, triggers, and foreign keys",
            set_up_script: &[
                "CREATE FUNCTION tag() RETURNS TEXT LANGUAGE plpgsql AS $$ BEGIN RETURN 'main'; END $$;",
                "CREATE FUNCTION ok(v INT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT v < 100 $$;",
                "CREATE FUNCTION twice(v INT) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT v * 2 $$;",
                "CREATE TABLE audit (msg TEXT);",
                "CREATE TABLE p (id INT PRIMARY KEY);",
                "CREATE TABLE c (id INT PRIMARY KEY, pid INT REFERENCES p ON DELETE CASCADE, v TEXT DEFAULT tag(), w INT CHECK (ok(w)));",
                "CREATE INDEX c_twice ON c (twice(w));",
                "CREATE FUNCTION bef() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN NEW.v := upper(NEW.v) || '-main'; RETURN NEW; END $$;",
                "CREATE FUNCTION aft() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN INSERT INTO audit VALUES ('main ' || NEW.id); RETURN NULL; END $$;",
                "CREATE TRIGGER c_bef BEFORE INSERT ON c FOR EACH ROW EXECUTE FUNCTION bef();",
                "CREATE TRIGGER c_aft AFTER INSERT ON c FOR EACH ROW WHEN (NEW.id > 0) EXECUTE FUNCTION aft();",
                "CREATE TABLE dp (id INT PRIMARY KEY);",
                "CREATE TABLE dc (id INT PRIMARY KEY, pid INT REFERENCES dp DEFERRABLE INITIALLY DEFERRED);",
                "INSERT INTO p VALUES (1);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_checkout('-b', 'b1');",
                "CREATE OR REPLACE FUNCTION tag() RETURNS TEXT LANGUAGE plpgsql AS $$ BEGIN RETURN 'b1'; END $$;",
                "CREATE OR REPLACE FUNCTION ok(v INT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT v < 10 $$;",
                "CREATE OR REPLACE FUNCTION twice(v INT) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT v * 3 $$;",
                "CREATE OR REPLACE FUNCTION bef() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN NEW.v := upper(NEW.v) || '-b1'; RETURN NEW; END $$;",
                "CREATE OR REPLACE FUNCTION aft() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN INSERT INTO audit VALUES ('b1 ' || NEW.id); RETURN NULL; END $$;",
                "INSERT INTO p VALUES (2);",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.c (id, pid, w) VALUES (10, 2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.c (id, pid, w) VALUES (11, 2, 50);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"c\" violates check constraint \"c_w_check\"", detail: "Failing row contains (11, 2, B1-b1, 50).", schema: "public", table: "c", constraint: "c_w_check", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.c (id, pid, w) VALUES (12, 3, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"c\" violates foreign key constraint \"c_pid_fkey\"", detail: "Key (pid)=(3) is not present in table \"p\".", schema: "public", table: "c", constraint: "c_pid_fkey", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO c (id, pid, w) VALUES (13, 2, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"c\" violates foreign key constraint \"c_pid_fkey\"", detail: "Key (pid)=(2) is not present in table \"p\".", schema: "public", table: "c", constraint: "c_pid_fkey", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT id, v, w FROM \"postgres/b1\".public.c;",
                    expected: Expected::Rows { columns: &[Column("id", INT4), Column("v", TEXT), Column("w", INT4)], rows: &[&[T("10"), T("B1-b1"), T("2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT msg FROM \"postgres/b1\".public.audit;",
                    expected: Expected::Rows { columns: &[Column("msg", TEXT)], rows: &[&[T("b1 10")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM audit;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT id FROM c WHERE twice(w) = 6;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("10")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/b1\".public.p WHERE id = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b1\".public.c;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.dc VALUES (1, 9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.dp VALUES (9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.dc VALUES (2, 8);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"dc\" violates foreign key constraint \"dc_pid_fkey\"", detail: "Key (pid)=(8) is not present in table \"dp\".", schema: "public", table: "dc", constraint: "dc_pid_fkey", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT id FROM \"postgres/b1\".public.dc ORDER BY id;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.dp VALUES (100);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO dp VALUES (200);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM \"postgres/b1\".public.dp WHERE id = 100) AS b1, (SELECT count(*) FROM dp WHERE id = 200) AS main;",
                    expected: Expected::Rows { columns: &[Column("b1", INT8), Column("main", INT8)], rows: &[&[T("0"), T("0")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.dp VALUES (100);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO dp VALUES (200);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM \"postgres/b1\".public.dp WHERE id = 100) AS b1, (SELECT count(*) FROM dp WHERE id = 200) AS main;",
                    expected: Expected::Rows { columns: &[Column("b1", INT8), Column("main", INT8)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch-qualified write checks the branch's domain constraints",
            set_up_script: &[
                "CREATE DOMAIN small AS INT CONSTRAINT small_max CHECK (VALUE < 100);",
                "CREATE TABLE d (v small);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_checkout('-b', 'b1');",
                "ALTER DOMAIN small DROP CONSTRAINT small_max;",
                "ALTER DOMAIN small ADD CONSTRAINT small_max CHECK (VALUE < 10);",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.d VALUES (50);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain small violates check constraint \"small_max\"", schema: "public", data_type: "small", constraint: "small_max", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "COPY FROM through a branch-qualified name loads the branch's working state",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "COPY \"postgres/b1\".public.test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-1pk5col-ints.csv",
                    skip: Some("revision_design §12: COPY resolves a branch-qualified name outside the statement overlay, so it refuses it as a cross-database reference"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM \"postgres/b1\".public.test) AS b1, (SELECT count(*) FROM test) AS main;",
                    expected: Expected::Rows { columns: &[Column("b1", INT8), Column("main", INT8)], rows: &[&[T("2"), T("0")]], tag: "SELECT 1" },
                    skip: Some("revision_design §12: COPY resolves a branch-qualified name outside the statement overlay, so it refuses it as a cross-database reference"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SET CONSTRAINTS naming a constraint of the session's branch leaves the same constraint of a branch written through its qualified name as it was",
            set_up_script: &[
                "CREATE TABLE dp (id INT PRIMARY KEY);",
                "CREATE TABLE dc (id INT PRIMARY KEY, pid INT CONSTRAINT dc_fk REFERENCES dp DEFERRABLE INITIALLY IMMEDIATE);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS dc_fk DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.dc VALUES (1, 9);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"dc\" violates foreign key constraint \"dc_fk\"", detail: "Key (pid)=(9) is not present in table \"dp\".", schema: "public", table: "dc", constraint: "dc_fk", ..E }),
                    skip: Some("revision_design §14: SET CONSTRAINTS keys its state by trigger OID, which a branch's copied trigger shares with the session branch's, so naming the session branch's constraint defers the branch's too"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.dc VALUES (2, 9);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "SET CONSTRAINTS naming constraints in a transaction that works on more than one branch is not supported yet", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch-qualified write checks the branch's domains over domains and its arrays' element domains, and refuses a column whose composite type holds a domain the branch defines differently",
            set_up_script: &[
                "CREATE DOMAIN small AS INT CONSTRAINT small_max CHECK (VALUE < 100);",
                "CREATE TABLE a (v small[]);",
                "CREATE TYPE pair AS (v small);",
                "CREATE TABLE c (p pair);",
                "CREATE DOMAIN d1 AS INT CONSTRAINT d1_max CHECK (VALUE < 100);",
                "CREATE DOMAIN d2 AS d1 CONSTRAINT d2_min CHECK (VALUE > 0);",
                "CREATE TABLE t2 (v d2);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_checkout('-b', 'b1');",
                "ALTER DOMAIN small DROP CONSTRAINT small_max;",
                "ALTER DOMAIN small ADD CONSTRAINT small_max CHECK (VALUE < 10) NOT VALID;",
                "ALTER DOMAIN d1 DROP CONSTRAINT d1_max;",
                "ALTER DOMAIN d1 ADD CONSTRAINT d1_max CHECK (VALUE < 10);",
                "ALTER DOMAIN d2 DROP CONSTRAINT d2_min;",
                "ALTER DOMAIN d2 ADD CONSTRAINT d2_min CHECK (VALUE > 5);",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t2 VALUES (50);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain d2 violates check constraint \"d1_max\"", schema: "public", data_type: "d2", constraint: "d1_max", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t2 VALUES (3);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain d2 violates check constraint \"d2_min\"", schema: "public", data_type: "d2", constraint: "d2_min", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t2 VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.a VALUES (ARRAY[50]);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain small violates check constraint \"small_max\"", schema: "public", data_type: "small", constraint: "small_max", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.c VALUES (ROW(50));",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain small violates check constraint \"small_max\"", schema: "public", data_type: "small", constraint: "small_max", ..E }),
                    skip: Some("revision_design §12: a copy takes the domains a column is and its array's element domains, not those its composite or range type holds, so it refuses those that differ"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.c VALUES (ROW(50));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "relation \"c\" has a column whose type contains a domain this branch defines differently", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b1\".public.a;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.a VALUES (ARRAY[5]);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a trigger of a branch's table written through its qualified name writes the branch's tables and fires their triggers once",
            set_up_script: &[
                "CREATE TABLE t (id INT);",
                "CREATE TABLE u (id INT);",
                "CREATE TABLE log (msg TEXT);",
                "CREATE FUNCTION t_aft() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN INSERT INTO u VALUES (NEW.id); RETURN NULL; END $$;",
                "CREATE FUNCTION u_aft() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN INSERT INTO log VALUES ('u ' || NEW.id); RETURN NULL; END $$;",
                "CREATE TRIGGER t_aft AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION t_aft();",
                "CREATE TRIGGER u_aft AFTER INSERT ON u FOR EACH ROW EXECUTE FUNCTION u_aft();",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM \"postgres/b1\".public.u) AS u, (SELECT string_agg(msg, ',') FROM \"postgres/b1\".public.log) AS log, (SELECT count(*) FROM u) AS main_u;",
                    expected: Expected::Rows { columns: &[Column("u", INT8), Column("log", TEXT), Column("main_u", INT8)], rows: &[&[T("1"), T("u 1"), T("0")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "one statement writing a branch's inheritance parent and its child through their qualified names writes the one child of the branch",
            set_up_script: &[
                "CREATE TABLE p (id INT PRIMARY KEY, v INT);",
                "CREATE TABLE c () INHERITS (p);",
                "INSERT INTO c VALUES (1, 0), (2, 0), (3, 0);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "WITH a AS (UPDATE \"postgres/b1\".public.p SET v = 100 WHERE id = 2 RETURNING id) UPDATE \"postgres/b1\".public.c SET v = 200 WHERE id = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT id, v FROM \"postgres/b1\".public.p ORDER BY id;",
                    expected: Expected::Rows { columns: &[Column("id", INT4), Column("v", INT4)], rows: &[&[T("1"), T("0")], &[T("2"), T("100")], &[T("3"), T("200")]], tag: "SELECT 3" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "WITH d AS (DELETE FROM \"postgres/b1\".public.c WHERE id = 1 RETURNING id) SELECT (SELECT count(*) FROM \"postgres/b1\".public.p) AS before, (SELECT count(*) FROM d) AS deleted;",
                    expected: Expected::Rows { columns: &[Column("before", INT8), Column("deleted", INT8)], rows: &[&[T("3"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM \"postgres/b1\".public.p) AS b1, (SELECT count(*) FROM p) AS main;",
                    expected: Expected::Rows { columns: &[Column("b1", INT8), Column("main", INT8)], rows: &[&[T("2"), T("3")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "WITH a AS (UPDATE \"postgres/b1\".public.p SET v = 101 WHERE id = 2 RETURNING id) UPDATE \"postgres/b1\".public.c SET v = 201 WHERE id = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE pid = pg_backend_pid() AND mode = 'RowExclusiveLock';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a trigger of a branch's table written through its qualified name runs a function unchanged on the branch against the branch's tables after the session ran it",
            set_up_script: &[
                "CREATE TABLE u (a INT);",
                "INSERT INTO u VALUES (1);",
                "CREATE TABLE t (id INT);",
                "CREATE TABLE log (msg TEXT);",
                "CREATE FUNCTION f() RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE r u%ROWTYPE; BEGIN SELECT * INTO r FROM u LIMIT 1; RETURN r::text; END $$;",
                "CREATE FUNCTION t_aft() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN INSERT INTO log VALUES (f() || ' ' || NEW.id); RETURN NULL; END $$;",
                "CREATE TRIGGER t_aft AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION t_aft();",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE u ADD COLUMN b TEXT DEFAULT 'x';",
                    expected: Expected::Ok,
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT string_agg(msg, ',') FROM log) AS main, (SELECT string_agg(msg, ',') FROM \"postgres/b1\".public.log) AS b1;",
                    expected: Expected::Rows { columns: &[Column("main", TEXT), Column("b1", TEXT)], rows: &[&[T("(1) 1"), T("(1,x) 2")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a function unchanged on a branch reads the branch's row type when the session runs it there after running it on its own branch",
            set_up_script: &[
                "CREATE TABLE u (a INT);",
                "INSERT INTO u VALUES (1);",
                "CREATE FUNCTION f() RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE r u%ROWTYPE; BEGIN SELECT * INTO r FROM u LIMIT 1; RETURN r::text; END $$;",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE u ADD COLUMN b TEXT DEFAULT 'x';",
                    expected: Expected::Ok,
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f();",
                    expected: Expected::Rows { columns: &[Column("f", TEXT)], rows: &[&[T("(1)")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT f();",
                    expected: Expected::Rows { columns: &[Column("f", TEXT)], rows: &[&[T("(1,x)")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch-qualified write checks CHECK constraints over the table's domain columns",
            set_up_script: &[
                "CREATE DOMAIN d AS INT CONSTRAINT d_max CHECK (VALUE < 100);",
                "CREATE TABLE e (v d CONSTRAINT e_v CHECK (v IS NOT NULL), w d CONSTRAINT e_w CHECK (w::INT < 50));",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.e VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.e VALUES (NULL, 2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"e\" violates check constraint \"e_v\"", detail: "Failing row contains (null, 2).", schema: "public", table: "e", constraint: "e_v", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.e VALUES (1, 60);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"e\" violates check constraint \"e_w\"", detail: "Failing row contains (1, 60).", schema: "public", table: "e", constraint: "e_w", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch-qualified write casts to the array of a domain by the branch's definition of the domain",
            set_up_script: &[
                "CREATE DOMAIN d AS INT CONSTRAINT d_max CHECK (VALUE < 100);",
                "CREATE TABLE e (v d, CONSTRAINT e_arr CHECK (cardinality(ARRAY[v]::d[]) = 1));",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
                "ALTER DOMAIN d DROP CONSTRAINT d_max;",
                "ALTER DOMAIN d ADD CONSTRAINT d_max CHECK (VALUE < 10);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.e VALUES (50);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.e VALUES (500);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain d violates check constraint \"d_max\"", schema: "public", data_type: "d", constraint: "d_max", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pg_typeof((ARRAY[v])[1]) = pg_typeof(v) AS same FROM \"postgres/b1\".public.e;",
                    expected: Expected::Rows { columns: &[Column("same", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch-qualified write's CHECK constraints and defaults that return a domain take the branch's domain",
            set_up_script: &[
                "CREATE DOMAIN dt AS TEXT CONSTRAINT dt_len CHECK (length(VALUE) < 10);",
                "CREATE DOMAIN dj AS JSON;",
                "CREATE TABLE j (v JSONB, w dt, CONSTRAINT j_c CHECK (JSON_VALUE(v, '$.a' RETURNING dt) IS NOT NULL));",
                "CREATE TABLE x (id INT, o dj DEFAULT JSON_OBJECT('a': 1 RETURNING dj));",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
                "ALTER DOMAIN dt DROP CONSTRAINT dt_len;",
                "ALTER DOMAIN dt ADD CONSTRAINT dt_len CHECK (length(VALUE) < 3);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.j VALUES ('{\"a\": \"hello\"}', NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.j VALUES ('{\"a\": \"hello world!\"}', NULL);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"j\" violates check constraint \"j_c\"", detail: "Failing row contains ({\"a\": \"hello world!\"}, null).", schema: "public", table: "j", constraint: "j_c", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.x (id) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT o::text AS o FROM \"postgres/b1\".public.x;",
                    expected: Expected::Rows { columns: &[Column("o", TEXT)], rows: &[&[T("{\"a\" : 1}")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch-qualified write's row comparison compares by the branch's btree support function",
            set_up_script: &[
                "CREATE FUNCTION my_cmp(a INT, b INT) RETURNS INT LANGUAGE SQL IMMUTABLE AS 'SELECT CASE WHEN a < b THEN -1 WHEN a > b THEN 1 ELSE 0 END';",
                "CREATE FUNCTION my_lt(a INT, b INT) RETURNS BOOL LANGUAGE SQL IMMUTABLE AS 'SELECT a > b';",
                "CREATE FUNCTION my_le(a INT, b INT) RETURNS BOOL LANGUAGE SQL IMMUTABLE AS 'SELECT a >= b';",
                "CREATE FUNCTION my_eq(a INT, b INT) RETURNS BOOL LANGUAGE SQL IMMUTABLE AS 'SELECT a = b';",
                "CREATE FUNCTION my_ge(a INT, b INT) RETURNS BOOL LANGUAGE SQL IMMUTABLE AS 'SELECT a <= b';",
                "CREATE FUNCTION my_gt(a INT, b INT) RETURNS BOOL LANGUAGE SQL IMMUTABLE AS 'SELECT a < b';",
                "CREATE OPERATOR <<< (LEFTARG = INT, RIGHTARG = INT, FUNCTION = my_lt);",
                "CREATE OPERATOR <<= (LEFTARG = INT, RIGHTARG = INT, FUNCTION = my_le);",
                "CREATE OPERATOR === (LEFTARG = INT, RIGHTARG = INT, FUNCTION = my_eq);",
                "CREATE OPERATOR >>= (LEFTARG = INT, RIGHTARG = INT, FUNCTION = my_ge);",
                "CREATE OPERATOR >>> (LEFTARG = INT, RIGHTARG = INT, FUNCTION = my_gt);",
                "CREATE OPERATOR CLASS rev_ops FOR TYPE INT USING btree AS OPERATOR 1 <<<, OPERATOR 2 <<=, OPERATOR 3 ===, OPERATOR 4 >>=, OPERATOR 5 >>>, FUNCTION 1 my_cmp(INT, INT);",
                "CREATE TABLE r (v INT, w INT, CONSTRAINT r_c CHECK ((v, w) <<< (10, 0)));",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION my_cmp(a INT, b INT) RETURNS INT LANGUAGE SQL IMMUTABLE AS 'SELECT CASE WHEN a < b THEN 1 WHEN a > b THEN -1 ELSE 0 END';",
                    expected: Expected::Ok,
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.r VALUES (5, 0);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"r\" violates check constraint \"r_c\"", detail: "Failing row contains (5, 0).", schema: "public", table: "r", constraint: "r_c", ..E }),
                    skip: Some("7402: the overlay copies no opfamily or amproc, so a branch-qualified write compares by main's support function"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_status_read_as_of_a_branch_leaves_the_sessions_own_status_to_the_next_query() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_status AS OF another branch and then unqualified in one session reads each branch's own status",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY)",
                "SELECT dolt_commit('-Am', 'one')",
                "SELECT dolt_checkout('-b', 'b1')",
                "INSERT INTO t VALUES (1)",
                "SELECT dolt_checkout('main')",
                "CREATE TABLE u (k INT PRIMARY KEY)",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'b1'",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[&[T("public.t"), T("f"), T("modified")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[&[T("public.u"), T("f"), T("new table")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'b1'",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[&[T("public.t"), T("f"), T("modified")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[&[T("public.u"), T("f"), T("new table")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_rows_read_as_of_a_commit_show_its_stamps() {
    run_scripts(&[
        ScriptTest {
            name: "a system catalog read as of a commit shows the commit's stamps while a running transaction locks the row on the branch",
            set_up_script: &[
                "CREATE TABLE st (a INT4);",
                "SELECT dolt_commit('-Am', 'st');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_class WHERE relname = 'st' FOR UPDATE;",
                    expected: Expected::Rows { columns: &[Column("relname", NAME)], rows: &[&[T("st")]], tag: "SELECT 1" },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT xmax::text <> '0' AS has_xmax FROM pg_class AS OF 'HEAD' WHERE relname = 'st';",
                    expected: Expected::Rows { columns: &[Column("has_xmax", BOOL)], rows: &[&[T("f")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT xmax::text <> '0' AS has_xmax FROM pg_class WHERE relname = 'st';",
                    expected: Expected::Rows { columns: &[Column("has_xmax", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_revision_qualified_calls_run_in_their_revision() {
    run_scripts(&[
        ScriptTest {
            name: "a revision-qualified CALL runs the revision's procedure in the revision, its writes going to that branch",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE PROCEDURE p(OUT r BIGINT) LANGUAGE plpgsql AS $$ BEGIN SELECT count(*) INTO r FROM t; END $$;",
                "CREATE PROCEDURE w() LANGUAGE plpgsql AS $$ BEGIN INSERT INTO t VALUES (5); END $$;",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT dolt_branch('b');",
                "SELECT dolt_tag('tag1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are the revision's (Appendix C).
                ScriptTestAssertion {
                    query: "CALL \"postgres/b\".public.w();",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/b\".public.p(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/main\".public.p(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/nosuch\".public.p(NULL);",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/tag1\".public.p(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/tag1\".public.w();",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a revision-qualified CALL inside a routine or DO block runs the branch's procedure, its plan following the branch's changes",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE PROCEDURE p(INOUT r BIGINT) LANGUAGE plpgsql AS $$ BEGIN SELECT count(*) INTO r FROM t; END $$;",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT dolt_branch('b');",
                "INSERT INTO t VALUES (2);",
                "CREATE FUNCTION f() RETURNS BIGINT LANGUAGE plpgsql AS $$ DECLARE r BIGINT; BEGIN CALL \"postgres/b\".public.p(r); RETURN r; END $$;",
                "CREATE PROCEDURE q(INOUT r BIGINT) LANGUAGE plpgsql AS $$ BEGIN CALL \"postgres/b\".public.p(r); END $$;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are the revision's (Appendix C).
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r BIGINT; BEGIN CALL \"postgres/b\".public.p(r); RAISE NOTICE 'b has %', r; END $$;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { code: "00000", message: "b has 1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f();",
                    expected: Expected::Rows {
                        columns: &[Column("f", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL q(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('b');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'b'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE PROCEDURE p(INOUT r BIGINT) LANGUAGE plpgsql AS $$ BEGIN SELECT count(*) + 100 INTO r FROM t; END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f();",
                    expected: Expected::Rows {
                        columns: &[Column("f", INT8)],
                        rows: &[
                            &[T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a revision-qualified CALL runs a SECURITY DEFINER procedure of the branch as its owner there with its search path, and names a missing one as written",
            set_up_script: &[
                "CREATE SCHEMA s;",
                "CREATE TABLE s.t (v INT);",
                "INSERT INTO s.t VALUES (7), (8), (9);",
                "CREATE ROLE owner_b;",
                "GRANT ALL ON SCHEMA s TO owner_b;",
                "GRANT SELECT ON s.t TO owner_b;",
                "CREATE PROCEDURE sd(INOUT who TEXT, INOUT n BIGINT) LANGUAGE plpgsql SECURITY DEFINER SET search_path = s AS $$ BEGIN who := current_user; SELECT count(*) INTO n FROM t; END $$;",
                "ALTER PROCEDURE sd(TEXT, BIGINT) OWNER TO owner_b;",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT dolt_branch('b');",
                "INSERT INTO s.t VALUES (10);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are the revision's (Appendix C).
                ScriptTestAssertion {
                    query: "CALL \"postgres/b\".public.sd(NULL, NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("who", TEXT), Column("n", INT8)],
                        rows: &[
                            &[T("owner_b"), T("3")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/b\".public.nosuch();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "procedure postgres/b.public.nosuch() does not exist", hint: "No procedure matches the given name and argument types. You might need to add explicit type casts.", position: 6, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/b\".public.sd(1, 2, 3);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "procedure postgres/b.public.sd(integer, integer, integer) does not exist", hint: "No procedure matches the given name and argument types. You might need to add explicit type casts.", position: 6, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/main\".public.nosuch();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "procedure postgres/main.public.nosuch() does not exist", hint: "No procedure matches the given name and argument types. You might need to add explicit type casts.", position: 6, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a revision-qualified CALL of a commit's procedure runs detached on that commit, apart from another commit with the same root, a CALL AS OF a commit runs the current procedure over its rows, and a kept plan follows a moved revision",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE PROCEDURE p(INOUT r TEXT) LANGUAGE plpgsql AS $$ BEGIN r := (SELECT count(*) FROM t)::text || ' ' || coalesce(active_branch(), 'none') || ' ' || (SELECT message FROM dolt_log LIMIT 1); END $$;",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT dolt_tag('v1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'second');",
                "DELETE FROM t WHERE v = 2;",
                "SELECT dolt_commit('-am', 'back');",
                "CREATE PROCEDURE q(INOUT r TEXT) LANGUAGE plpgsql AS $$ BEGIN CALL p(r) AS OF 'HEAD~1'; END $$;",
                "SELECT dolt_commit('-Am', 'q');",
                "SELECT dolt_tag('v2');",
                "CREATE FUNCTION f() RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE r TEXT; BEGIN CALL p(r) AS OF 'HEAD'; RETURN r; END $$;",
                "CREATE FUNCTION g() RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE r TEXT; BEGIN CALL \"postgres/v1\".public.p(r); RETURN r; END $$;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are the revision's (Appendix C).
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_db('v1') = dolt_hashof_db('HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p(NULL) AS OF 'v1';",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("1 main first")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p(NULL) AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("1 main back")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/v1\".public.p(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("1 none first")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/v2\".public.q(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("1 none back")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("1 none q")],
                        ],
                        tag: "CALL",
                    },
                    client: "v2",
                    database: "postgres/v2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f(), g();",
                    expected: Expected::Rows {
                        columns: &[Column("f", TEXT), Column("g", TEXT)],
                        rows: &[
                            &[T("1 main q"), T("1 none first")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'third') IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('-d', 'v1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f(), g();",
                    expected: Expected::Rows {
                        columns: &[Column("f", TEXT), Column("g", TEXT)],
                        rows: &[
                            &[T("2 main third"), T("2 none third")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a CALL AS OF a bound parameter reads each bound revision with the current procedure, and a routine's variable names a revision",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE PROCEDURE p(INOUT r INT) LANGUAGE plpgsql AS $$ BEGIN SELECT v * 10 INTO r FROM t; END $$;",
                "SELECT dolt_commit('-Am', 'one');",
                "CREATE OR REPLACE PROCEDURE p(INOUT r INT) LANGUAGE plpgsql AS $$ BEGIN SELECT v * 100 INTO r FROM t; END $$;",
                "UPDATE t SET v = 2;",
                "SELECT dolt_commit('-Am', 'two');",
                "CREATE FUNCTION f(rev TEXT) RETURNS INT LANGUAGE plpgsql AS $$ DECLARE r INT; BEGIN CALL p(r) AS OF rev; RETURN r; END $$;",
                "CREATE FUNCTION g(rev TEXT) RETURNS BIGINT LANGUAGE plpgsql AS $$ BEGIN RETURN (SELECT count(*) FROM t AS OF rev); END $$;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are the revision's (Appendix C).
                ScriptTestAssertion {
                    query: "CALL p(NULL) AS OF $1;",
                    expected: Expected::Tag(""),
                    client: "c",
                    prepare: "callrev",
                    ..A
                },
                ScriptTestAssertion {
                    query: "callrev",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "CALL",
                    },
                    client: "c",
                    bind_vars: &[BindVar::Str("HEAD~1")],
                    ..A
                },
                ScriptTestAssertion {
                    query: "callrev",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "CALL",
                    },
                    client: "c",
                    bind_vars: &[BindVar::Str("HEAD")],
                    ..A
                },
                ScriptTestAssertion {
                    query: "callrev",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "CALL",
                    },
                    client: "c",
                    bind_vars: &[BindVar::Str("HEAD~1")],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f('HEAD~1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: rev", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g('HEAD~1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: rev", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a SECURITY DEFINER procedure of a tag or branch that sets its search path leaves the caller's path as it was",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE SCHEMA s;",
                "CREATE TABLE s.t (v INT);",
                "INSERT INTO s.t VALUES (7), (8);",
                "CREATE ROLE owner_s;",
                "GRANT ALL ON SCHEMA s TO owner_s;",
                "GRANT SELECT ON s.t TO owner_s;",
                "CREATE PROCEDURE sd(INOUT n BIGINT) LANGUAGE plpgsql SECURITY DEFINER SET search_path = s AS $$ BEGIN SELECT count(*) INTO n FROM t; END $$;",
                "ALTER PROCEDURE sd(BIGINT) OWNER TO owner_s;",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT dolt_tag('tag1');",
                "SELECT dolt_branch('b');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected rows are the revision's (Appendix C).
                ScriptTestAssertion {
                    query: "CALL \"postgres/tag1\".public.sd(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), current_user FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("current_user", NAME)],
                        rows: &[
                            &[T("1"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/b\".public.sd(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), current_user FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("current_user", NAME)],
                        rows: &[
                            &[T("1"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "calls through two revision qualifiers of one commit follow each qualifier when one moves",
            intent: "Two qualified CALLs that name one commit through different tags each resolve again by their own qualifier, so moving one tag changes only the CALL through it.",
            set_up_script: &[
                "CREATE TABLE t (v INT);",
                "INSERT INTO t VALUES (1);",
                "CREATE PROCEDURE p(INOUT r TEXT) LANGUAGE plpgsql AS $$ BEGIN r := (SELECT count(*) FROM t)::text; END $$;",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_tag('v1', 'HEAD');",
                "SELECT dolt_tag('v2', 'HEAD');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-Am', 'two');",
                "CREATE FUNCTION g() RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE a TEXT; b TEXT; BEGIN CALL \"postgres/v1\".public.p(a); CALL \"postgres/v2\".public.p(b); RETURN a || ' ' || b; END $$;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT g();",
                    expected: Expected::Rows {
                        columns: &[Column("g", TEXT)],
                        rows: &[
                            &[T("1 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('-d', 'v1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g();",
                    expected: Expected::Rows {
                        columns: &[Column("g", TEXT)],
                        rows: &[
                            &[T("2 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_revision_qualified_names_read_as_of_revisions() {
    run_scripts(&[
        ScriptTest {
            name: "a revision-qualified name read AS OF a revision resolves HEAD, WORKING, and STAGED in the qualifier's branch",
            intent: "As Dolt resolves AS OF in a revision database, HEAD and its ancestors, WORKING, and STAGED name the qualifier's branch's own states, and a branch, tag, or commit names itself.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v text);",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_tag('v1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2, 'two');",
                "SELECT dolt_commit('--branch', 'b1', '-am', 'c2');",
                "INSERT INTO \"postgres/b1\".public.t VALUES (3, 'three');",
                "SELECT dolt_add('--branch', 'b1', 't');",
                "INSERT INTO \"postgres/b1\".public.t VALUES (4, 'four');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1,2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1\".public.t AS OF 'HEAD~1';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1\".public.t AS OF 'WORKING';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1,2,3,4")]], tag: "SELECT 1" },
                    dolt: Some(Dolt::Differs("Dolt resolves WORKING and STAGED on a revision-qualified name in the session's branch while it resolves HEAD in the qualifier's, where Doltgrust resolves every relative revision in the qualifier's")),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1\".public.t AS OF 'STAGED';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1,2,3")]], tag: "SELECT 1" },
                    dolt: Some(Dolt::Differs("Dolt resolves WORKING and STAGED on a revision-qualified name in the session's branch while it resolves HEAD in the qualifier's, where Doltgrust resolves every relative revision in the qualifier's")),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1\".public.t AS OF 'main';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1\".public.t AS OF 'v1';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/v1\".public.t AS OF 'STAGED';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1~1\".public.t AS OF 'STAGED';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b1\".public.t AS OF '2999-01-01'::timestamptz;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM t AS OF 'HEAD';",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"other/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cross-database references are not implemented", position: 22, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a revision-qualified name read AS OF a revision bound at execution",
            intent: "A revision-qualified name read AS OF a parameter resolves the parameter's revision in the qualifier's branch.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v text);",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_tag('v1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2, 'two');",
                "SELECT dolt_commit('--branch', 'b1', '-am', 'c2');",
                "INSERT INTO \"postgres/b1\".public.t VALUES (3, 'three');",
                "SELECT dolt_add('--branch', 'b1', 't');",
                "INSERT INTO \"postgres/b1\".public.t VALUES (4, 'four');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1\".public.t AS OF $1;",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    bind_vars: &[BindVar::Str("HEAD~1")],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT string_agg(pk::text, ',' ORDER BY pk) AS pks FROM \"postgres/b1\".public.t AS OF '2999-01-01'::timestamptz;",
                    expected: Expected::Rows { columns: &[Column("pks", TEXT)], rows: &[&[T("1,2")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_revision_qualified_names_read_as_of_a_time() {
    run_scripts(&[
        ScriptTest {
            name: "a revision-qualified name read AS OF a time finds the time in the qualifier's history, in views, stored views, and prepared statements",
            set_up_script: &[
                "SET TIME ZONE 'UTC';",
                "CREATE TABLE t (pk INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 10);",
                "CREATE VIEW tv AS SELECT v FROM t;",
                "SELECT dolt_commit('-Am', 'one', '--date', '2026-01-01T00:00:00Z');",
                "SELECT dolt_branch('b');",
                "UPDATE \"postgres/b\".public.t SET v = 20;",
                "SELECT dolt_commit('--branch', 'b', '-am', 'b two', '--date', '2026-02-01T00:00:00Z');",
                "UPDATE \"postgres/b\".public.t SET v = 30;",
                "SELECT dolt_commit('--branch', 'b', '-am', 'b three', '--date', '2026-03-01T00:00:00Z');",
                "UPDATE t SET v = 40;",
                "SELECT dolt_commit('-am', 'main two', '--date', '2026-02-15T00:00:00Z');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM \"postgres/b\".public.t AS OF '2026-02-20 00:00:00+00'::timestamptz;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("20")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM t AS OF '2026-02-20 00:00:00+00'::timestamptz;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("40")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM \"postgres/b\".public.tv AS OF '2026-03-02 00:00:00+00'::timestamptz;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("30")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b\".public.t AS OF '2025-01-01 00:00:00+00'::timestamptz;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"public.t\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW w AS SELECT v FROM \"postgres/b\".public.t AS OF '2026-02-20 00:00:00+00'::timestamptz;",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM w;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("20")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('w');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[&[T(" SELECT v\n   FROM \"postgres/b\".public.t AS OF '2026-02-20 00:00:00+00'::timestamp with time zone;")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM \"postgres/b\".public.t AS OF CAST($1 AS timestamptz);",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("30")]], tag: "SELECT 1" },
                    bind_vars: &[BindVar::Str("2026-03-02 00:00:00+00")],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_relations_only_at_a_revision_read_as_of_a_parameter() {
    run_scripts(&[
        ScriptTest {
            name: "a relation the session's branch lacks, read AS OF a revision bound at execution, through a plain or a revision-qualified name",
            set_up_script: &[
                "CREATE TABLE gone (v INT);",
                "INSERT INTO gone VALUES (1);",
                "SELECT dolt_commit('-Am', 'one');",
                "DROP TABLE gone;",
                "SELECT dolt_commit('-Am', 'two');",
                "SELECT dolt_branch('b');",
                "CREATE TABLE \"postgres/b\".public.onlyb (v INT);",
                "INSERT INTO \"postgres/b\".public.onlyb VALUES (1);",
                "SELECT dolt_commit('--branch', 'b', '-Am', 'b one');",
                "UPDATE \"postgres/b\".public.onlyb SET v = 2;",
                "SELECT dolt_commit('--branch', 'b', '-am', 'b two');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM gone AS OF $1;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    bind_vars: &[BindVar::Str("HEAD~1")],
                    skip: Some("AS OF a parameter needs the relation in the session's branch, since analysis takes the statement's columns from it before the value is bound"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM \"postgres/b\".public.onlyb AS OF $1;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    bind_vars: &[BindVar::Str("HEAD~1")],
                    skip: Some("AS OF a parameter needs the relation in the session's branch, since analysis takes the statement's columns from it before the value is bound"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_sql_functions_whose_bodies_read_as_of() {
    run_scripts(&[
        ScriptTest {
            name: "a SQL function whose body reads a relation AS OF a revision or through a revision-qualified name runs without being inlined",
            set_up_script: &[
                "CREATE TABLE u (pk INT PRIMARY KEY, x INT);",
                "INSERT INTO u VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'u1');",
                "UPDATE u SET x = 2;",
                "SELECT dolt_commit('-Am', 'u2');",
                "CREATE FUNCTION fs() RETURNS TEXT LANGUAGE sql AS $$ SELECT row_to_json(q)::text FROM (SELECT * FROM u AS OF 'HEAD~1') q $$;",
                "CREATE FUNCTION fsr() RETURNS SETOF u LANGUAGE sql AS $$ SELECT * FROM u AS OF 'HEAD~1' $$;",
                "CREATE FUNCTION fsb() RETURNS SETOF u LANGUAGE sql BEGIN ATOMIC SELECT * FROM u AS OF 'HEAD~1'; END;",
                "CREATE FUNCTION fsq() RETURNS INT LANGUAGE sql AS $$ SELECT x FROM \"postgres/main\".public.u AS OF 'HEAD~1' $$;",
                "CREATE VIEW v AS SELECT * FROM u AS OF 'HEAD~1';",
                "CREATE FUNCTION fv() RETURNS SETOF u LANGUAGE sql AS $$ SELECT * FROM v $$;",
                "CREATE FUNCTION fvc() RETURNS INT LANGUAGE sql AS $$ SELECT count(*)::int FROM v $$;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT fs();",
                    expected: Expected::Rows { columns: &[Column("fs", TEXT)], rows: &[&[T("{\"pk\":1,\"x\":1}")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM fsr() WHERE x > 0;",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("x", INT4)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM fsb();",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("x", INT4)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT fsq();",
                    expected: Expected::Rows { columns: &[Column("fsq", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM fv();",
                    expected: Expected::Rows { columns: &[Column("pk", INT4), Column("x", INT4)], rows: &[&[T("1"), T("1")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT fvc();",
                    expected: Expected::Rows { columns: &[Column("fvc", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
