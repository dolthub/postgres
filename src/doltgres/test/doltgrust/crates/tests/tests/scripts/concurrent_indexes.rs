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
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};


#[test]
fn test_create_index_concurrently() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE INDEX CONCURRENTLY builds an index queries use, and a unique one that finds a duplicate is left invalid",
            set_up_script: &[
                "CREATE TABLE c (id INT PRIMARY KEY, a INT, b TEXT);",
                "INSERT INTO c SELECT i, i % 10, 'x' || i FROM generate_series(1, 1000) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX CONCURRENTLY c_a ON c (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'c_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM c WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY c_b ON c (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES (1001, 1, 'x1');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"c_b\"", detail: "Key (b)=(x1) already exists.", schema: "public", table: "c", constraint: "c_b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY c_a_u ON c (a);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"c_a_u\"", detail: "Key (a)=(1) is duplicated.", schema: "public", table: "c", constraint: "c_a_u", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'c_a_u'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM c WHERE a = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX c_a_u;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX CONCURRENTLY IF NOT EXISTS c_a ON c (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    notices: &[Diagnostic { code: "42P07", message: r#"relation "c_a" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX CONCURRENTLY c_x ON c (b);",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CREATE INDEX CONCURRENTLY cannot run inside a transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE tt (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX CONCURRENTLY tt_a ON tt (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'tt_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE INDEX CONCURRENTLY waits for the table's writers, and its entries cover rows they changed before and after the index was ready",
            set_up_script: &[
                "CREATE TABLE w (id INT PRIMARY KEY, a INT);",
                "INSERT INTO w SELECT i, i FROM generate_series(1, 100) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO w VALUES (101, 101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE INDEX CONCURRENTLY w_a ON w (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ UPDATE w SET a = 1000 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ INSERT INTO w VALUES (102, 102);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ UPDATE w SET a = 2000 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ DELETE FROM w WHERE id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'w_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM w WHERE a IN (1, 2, 3, 101, 102, 1000, 2000) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("101")],
                            &[T("102")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM w WHERE a > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "CREATE INDEX CONCURRENTLY on a table without a primary key lets a writer go on while it waits for older writers",
            set_up_script: &[
                "CREATE TABLE kl (a INT, b INT);",
                "INSERT INTO kl SELECT i, i FROM generate_series(1, 100) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO kl VALUES (101, 101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE INDEX CONCURRENTLY kl_a ON kl (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO kl VALUES (102, 102);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ INSERT INTO kl VALUES (103, 103);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'kl_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM kl WHERE a > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("103")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE UNIQUE INDEX CONCURRENTLY accepts a key a row moved off while the index was built",
            set_up_script: &[
                "CREATE TABLE u (id INT PRIMARY KEY, b TEXT);",
                "INSERT INTO u VALUES (1, 'x'), (2, 'z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO u VALUES (3, 'q');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE UNIQUE INDEX CONCURRENTLY u_b ON u (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO u VALUES (4, 'r');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client W */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "W",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client W */ UPDATE u SET b = 'y' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "W",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client W */ INSERT INTO u VALUES (5, 'x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "W",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client W */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "W",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'u_b'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM u WHERE b IN ('x', 'y') ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (6, 'y');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"u_b\"", detail: "Key (b)=(y) already exists.", schema: "public", table: "u", constraint: "u_b", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE UNIQUE INDEX CONCURRENTLY ordered by an operator class's comparison function lets a writer go on while it waits for older writers",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(a INT, b INT) RETURNS INT IMMUTABLE LANGUAGE sql AS $$ SELECT CASE WHEN a < b THEN 1 WHEN a > b THEN -1 ELSE 0 END $$;",
                "CREATE OPERATOR CLASS rev_ops FOR TYPE INT USING btree AS OPERATOR 1 >, OPERATOR 2 >=, OPERATOR 3 =, OPERATOR 4 <=, OPERATOR 5 <, FUNCTION 1 rev_cmp(INT, INT);",
                "CREATE TABLE cu (id INT PRIMARY KEY, a INT);",
                "INSERT INTO cu SELECT i, i FROM generate_series(1, 100) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO cu VALUES (101, 101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE UNIQUE INDEX CONCURRENTLY cu_a ON cu (a rev_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO cu VALUES (102, 102);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ INSERT INTO cu VALUES (103, 103);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'cu_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM cu WHERE a > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("103")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE UNIQUE INDEX CONCURRENTLY finds keys its operator class's comparison function makes equal though their bytes differ",
            set_up_script: &[
                "CREATE FUNCTION abs_cmp(a INT, b INT) RETURNS INT IMMUTABLE LANGUAGE sql AS $$ SELECT CASE WHEN abs(a) < abs(b) THEN -1 WHEN abs(a) > abs(b) THEN 1 ELSE 0 END $$;",
                "CREATE FUNCTION abs_lt(a INT, b INT) RETURNS BOOL IMMUTABLE LANGUAGE sql AS $$ SELECT abs(a) < abs(b) $$;",
                "CREATE FUNCTION abs_le(a INT, b INT) RETURNS BOOL IMMUTABLE LANGUAGE sql AS $$ SELECT abs(a) <= abs(b) $$;",
                "CREATE FUNCTION abs_eq(a INT, b INT) RETURNS BOOL IMMUTABLE LANGUAGE sql AS $$ SELECT abs(a) = abs(b) $$;",
                "CREATE FUNCTION abs_ge(a INT, b INT) RETURNS BOOL IMMUTABLE LANGUAGE sql AS $$ SELECT abs(a) >= abs(b) $$;",
                "CREATE FUNCTION abs_gt(a INT, b INT) RETURNS BOOL IMMUTABLE LANGUAGE sql AS $$ SELECT abs(a) > abs(b) $$;",
                "CREATE OPERATOR <| (LEFTARG = INT, RIGHTARG = INT, FUNCTION = abs_lt);",
                "CREATE OPERATOR <=| (LEFTARG = INT, RIGHTARG = INT, FUNCTION = abs_le);",
                "CREATE OPERATOR =| (LEFTARG = INT, RIGHTARG = INT, FUNCTION = abs_eq);",
                "CREATE OPERATOR >=| (LEFTARG = INT, RIGHTARG = INT, FUNCTION = abs_ge);",
                "CREATE OPERATOR >| (LEFTARG = INT, RIGHTARG = INT, FUNCTION = abs_gt);",
                "CREATE OPERATOR CLASS abs_ops FOR TYPE INT USING btree AS OPERATOR 1 <|, OPERATOR 2 <=|, OPERATOR 3 =|, OPERATOR 4 >=|, OPERATOR 5 >|, FUNCTION 1 abs_cmp(INT, INT);",
                "CREATE TABLE ab (id INT PRIMARY KEY, a INT);",
                "INSERT INTO ab VALUES (1, 5), (2, 7), (3, -5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY ab_a ON ab (a abs_ops);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"ab_a\"", detail: "Key (a)=(5) is duplicated.", schema: "public", table: "ab", constraint: "ab_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'ab_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ab WHERE id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX CONCURRENTLY ab_a;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'ab_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ab VALUES (4, -7);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"ab_a\"", detail: "Key (a)=(-7) already exists.", schema: "public", table: "ab", constraint: "ab_a", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE INDEX CONCURRENTLY on a table without a primary key lets writers insert, update, and delete rows and copies while it waits between its phases",
            set_up_script: &[
                "CREATE TABLE kw (a INT, b INT);",
                "INSERT INTO kw SELECT i, i FROM generate_series(1, 100) i;",
                "INSERT INTO kw VALUES (5, 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO kw VALUES (101, 101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE INDEX CONCURRENTLY kw_b ON kw (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    client: "B",
                    flow: Flow::Exec,
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO kw VALUES (102, 102);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ INSERT INTO kw VALUES (5, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ UPDATE kw SET b = 1007 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ DELETE FROM kw WHERE a = 9;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'kw_b'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM kw WHERE b > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("103")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(*) FROM kw WHERE b IN (5, 7, 9, 101, 102, 103, 1007) GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("5"), T("3")],
                            &[T("101"), T("1")],
                            &[T("102"), T("1")],
                            &[T("1007"), T("1")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE INDEX CONCURRENTLY on a table keyed by a deferrable primary key lets writers insert, update, and delete rows while it waits between its phases",
            set_up_script: &[
                "CREATE TABLE kp (id INT PRIMARY KEY DEFERRABLE, b INT);",
                "INSERT INTO kp SELECT i, i FROM generate_series(1, 100) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO kp VALUES (101, 101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE INDEX CONCURRENTLY kp_b ON kp (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    client: "B",
                    flow: Flow::Exec,
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO kp VALUES (102, 102);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ INSERT INTO kp VALUES (103, 103);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ UPDATE kp SET b = 1007 WHERE id = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ DELETE FROM kp WHERE id = 9;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'kp_b'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM kp WHERE b > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("102")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(*) FROM kp WHERE b IN (5, 7, 9, 101, 102, 103, 1007) GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("5"), T("1")],
                            &[T("101"), T("1")],
                            &[T("102"), T("1")],
                            &[T("103"), T("1")],
                            &[T("1007"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE UNIQUE INDEX CONCURRENTLY on a table without a primary key fails for a key two rows or two copies of a row share, leaving the index invalid",
            set_up_script: &[
                "CREATE TABLE ku (a INT, b INT);",
                "INSERT INTO ku SELECT i, i FROM generate_series(1, 50) i;",
                "INSERT INTO ku VALUES (7, 7);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY ku_a ON ku (a);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"ku_a\"", detail: "Key (a)=(7) is duplicated.", schema: "public", table: "ku", constraint: "ku_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'ku_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (7, 70);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (51, 51);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (51, 52);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ku SET a = 51 WHERE a = 50;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, count(*) FROM ku WHERE a IN (7, 50, 51) GROUP BY a ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("7"), T("3")],
                            &[T("51"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX ku_a;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ku WHERE a IN (7, 51);",
                    expected: Expected::Tag("DELETE 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (7, 7), (8, 80);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY ku_a ON ku (a);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"ku_a\"", detail: "Key (a)=(8) is duplicated.", schema: "public", table: "ku", constraint: "ku_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'ku_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX ku_a;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ku WHERE a = 8 AND b = 80;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY ku_a ON ku (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'ku_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM ku WHERE a BETWEEN 6 AND 8 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("6"), T("6")],
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ku;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_drop_index_concurrently() {
    run_scripts(&[
        ScriptTest {
            name: "DROP INDEX CONCURRENTLY drops one index, with Postgres' refusals, and event triggers see it dropped once",
            set_up_script: &[
                "CREATE TABLE d (id INT PRIMARY KEY, a INT, b INT);",
                "INSERT INTO d SELECT i, i, i FROM generate_series(1, 100) i;",
                "CREATE INDEX d_a ON d (a);",
                "CREATE INDEX d_b ON d (b);",
                "ALTER TABLE d ADD CONSTRAINT d_b_u UNIQUE (b);",
                "CREATE FUNCTION dropped() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_dropped_objects() LOOP RAISE NOTICE 'dropped % %', r.object_type, r.object_identity; END LOOP; END $$;",
                "CREATE FUNCTION f() RETURNS VOID LANGUAGE plpgsql AS $$ BEGIN DROP INDEX CONCURRENTLY d_a; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY d_a, d_b;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DROP INDEX CONCURRENTLY does not support dropping multiple objects", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY d_a CASCADE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DROP INDEX CONCURRENTLY does not support CASCADE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY d_a;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "DROP INDEX CONCURRENTLY cannot run inside a transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY d_b_u;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop index d_b_u because constraint d_b_u on table d requires it", hint: "You can drop constraint d_b_u on table d instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY IF EXISTS nope;",
                    expected: Expected::Tag("DROP INDEX"),
                    notices: &[Diagnostic { code: "00000", message: r#"index "nope" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f();",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "DROP INDEX CONCURRENTLY cannot be executed from a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER et ON sql_drop EXECUTE FUNCTION dropped();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY d_a;",
                    expected: Expected::Tag("DROP INDEX"),
                    notices: &[Diagnostic { code: "00000", message: r#"dropped index public.d_a"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER et;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'd_a';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE tt (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX tt_a ON tt (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY tt_a;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM d WHERE b > 50;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP INDEX CONCURRENTLY waits for the transactions using the table, while they and new ones go on",
            set_up_script: &[
                "CREATE TABLE r (id INT PRIMARY KEY, a INT);",
                "INSERT INTO r SELECT i, i FROM generate_series(1, 100) i;",
                "CREATE INDEX r_a ON r (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM r WHERE a < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DROP INDEX CONCURRENTLY r_a;",
                    expected: Expected::Tag("DROP INDEX"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO r VALUES (101, 101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT count(*) FROM r WHERE a < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM r WHERE a > 95;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pg_class WHERE relname = 'r_a';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_reindex_concurrently() {
    run_scripts(&[
        ScriptTest {
            name: "REINDEX CONCURRENTLY rebuilds a table's indexes in place, keeping their names, comments, constraints, and statistics",
            set_up_script: &[
                "CREATE TABLE p (id INT PRIMARY KEY, code TEXT UNIQUE, a INT, b INT);",
                "INSERT INTO p SELECT i, 'c' || i, i % 7, i FROM generate_series(1, 200) i;",
                "CREATE INDEX p_a ON p (a);",
                "CREATE INDEX p_ab ON p (a, b) WHERE b > 10;",
                "COMMENT ON INDEX p_a IS 'the a index';",
                "CREATE TABLE ch (id INT PRIMARY KEY, code TEXT REFERENCES p (code));",
                "INSERT INTO ch VALUES (1, 'c5');",
                "CREATE TABLE concur_exprs_tab (c1 INT, c2 BOOLEAN);",
                "INSERT INTO concur_exprs_tab (c1, c2) VALUES (1369652450, FALSE), (414515746, TRUE), (897778963, FALSE);",
                "CREATE UNIQUE INDEX concur_exprs_index_expr ON concur_exprs_tab ((c1::text COLLATE \"C\"));",
                "ALTER INDEX concur_exprs_index_expr ALTER COLUMN 1 SET STATISTICS 100;",
                "ANALYZE concur_exprs_tab;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "REINDEX TABLE CONCURRENTLY p;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, i.indisvalid, i.indisready, i.indisprimary FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 'p'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("indisvalid", BOOL), Column("indisready", BOOL), Column("indisprimary", BOOL)],
                        rows: &[
                            &[T("p_a"), T("t"), T("t"), T("f")],
                            &[T("p_ab"), T("t"), T("t"), T("f")],
                            &[T("p_code_key"), T("t"), T("t"), T("f")],
                            &[T("p_pkey"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('p_a'::regclass, 'pg_class');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("the a index")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conindid::regclass FROM pg_constraint WHERE conrelid IN ('p'::regclass, 'ch'::regclass) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conindid", REGCLASS)],
                        rows: &[
                            &[T("ch_code_fkey"), T("p_code_key")],
                            &[T("ch_id_not_null"), T("-")],
                            &[T("ch_pkey"), T("ch_pkey")],
                            &[T("p_code_key"), T("p_code_key")],
                            &[T("p_id_not_null"), T("-")],
                            &[T("p_pkey"), T("p_pkey")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ch VALUES (2, 'nope');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"ch\" violates foreign key constraint \"ch_code_fkey\"", detail: "Key (code)=(nope) is not present in table \"p\".", schema: "public", table: "ch", constraint: "ch_code_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (201, 'c1', 0, 0);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"p_code_key\"", detail: "Key (code)=(c1) already exists.", schema: "public", table: "p", constraint: "p_code_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM p WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("29")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM p WHERE a = 3 AND b > 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("27")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX CONCURRENTLY p_a;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('p_ab'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T("CREATE INDEX p_ab ON public.p USING btree (a, b) WHERE (b > 10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT starelid::regclass, count(*) FROM pg_statistic WHERE starelid IN ('concur_exprs_index_expr'::regclass) GROUP BY starelid;",
                    expected: Expected::Rows {
                        columns: &[Column("starelid", REGCLASS), Column("count", INT8)],
                        rows: &[
                            &[T("concur_exprs_index_expr"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE CONCURRENTLY concur_exprs_tab;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT starelid::regclass, count(*) FROM pg_statistic WHERE starelid IN ('concur_exprs_index_expr'::regclass) GROUP BY starelid;",
                    expected: Expected::Rows {
                        columns: &[Column("starelid", REGCLASS), Column("count", INT8)],
                        rows: &[
                            &[T("concur_exprs_index_expr"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attrelid::regclass, attnum, attstattarget FROM pg_attribute WHERE attrelid = 'concur_exprs_index_expr'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("attrelid", REGCLASS), Column("attnum", INT2), Column("attstattarget", INT2)],
                        rows: &[
                            &[T("concur_exprs_index_expr"), T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE CONCURRENTLY p;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "REINDEX CONCURRENTLY cannot run inside a transaction block", ..E }),
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
            name: "REINDEX CONCURRENTLY waits for the table's writers, and its new indexes cover what they wrote",
            set_up_script: &[
                "CREATE TABLE rw (id INT PRIMARY KEY, a INT);",
                "INSERT INTO rw SELECT i, i FROM generate_series(1, 100) i;",
                "CREATE INDEX rw_a ON rw (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE rw SET a = 1000 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ REINDEX INDEX CONCURRENTLY rw_a;",
                    expected: Expected::Tag("REINDEX"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO rw VALUES (101, 101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pg_class WHERE relname LIKE 'rw_a%';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rw WHERE a IN (1, 101, 1000) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("101")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REINDEX CONCURRENTLY of a deferrable unique constraint's index lets a writer go on while it waits for older writers",
            set_up_script: &[
                "CREATE TABLE du (id INT PRIMARY KEY, a INT, CONSTRAINT du_a UNIQUE (a) DEFERRABLE);",
                "INSERT INTO du SELECT i, i FROM generate_series(1, 100) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO du VALUES (101, 101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ REINDEX INDEX CONCURRENTLY du_a;",
                    expected: Expected::Tag("REINDEX"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO du VALUES (102, 102);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ INSERT INTO du VALUES (103, 103);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indisready, indisvalid FROM pg_index WHERE indexrelid = 'du_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisready", BOOL), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM du WHERE a > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("103")],
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
fn test_concurrently_event_triggers() {
    run_scripts(&[
        ScriptTest {
            name: "CONCURRENTLY commands fire their ddl_command_end and sql_drop event triggers once they finish, and not when they fail",
            set_up_script: &[
                "CREATE TABLE e (id INT PRIMARY KEY, a INT);",
                "INSERT INTO e VALUES (1, 1), (2, 1);",
                "CREATE FUNCTION ended() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_ddl_commands() LOOP RAISE NOTICE 'end % % valid=%', r.command_tag, r.object_identity, (SELECT indisvalid FROM pg_index WHERE indexrelid = r.objid); END LOOP; END $$;",
                "CREATE FUNCTION dropped() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_dropped_objects() LOOP RAISE NOTICE 'dropped % % gone=%', r.object_type, r.object_identity, NOT EXISTS (SELECT 1 FROM pg_class WHERE oid = r.objid); END LOOP; END $$;",
                "CREATE FUNCTION started() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'start %', tg_tag; END $$;",
                "CREATE EVENT TRIGGER s ON ddl_command_start EXECUTE FUNCTION started();",
                "CREATE EVENT TRIGGER x ON ddl_command_end EXECUTE FUNCTION ended();",
                "CREATE EVENT TRIGGER y ON sql_drop EXECUTE FUNCTION dropped();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX CONCURRENTLY e_a ON e (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    notices: &[Diagnostic { code: "00000", message: r#"start CREATE INDEX"#, ..N }, Diagnostic { code: "00000", message: r#"end CREATE INDEX public.e_a valid=t"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY e_a_u ON e (a);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"e_a_u\"", detail: "Key (a)=(1) is duplicated.", schema: "public", table: "e", constraint: "e_a_u", ..E }),
                    notices: &[Diagnostic { code: "00000", message: r#"start CREATE INDEX"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX e_a_u;",
                    expected: Expected::Tag("DROP INDEX"),
                    notices: &[Diagnostic { code: "00000", message: r#"start DROP INDEX"#, ..N }, Diagnostic { code: "00000", message: r#"dropped index public.e_a_u gone=t"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY e_a;",
                    expected: Expected::Tag("DROP INDEX"),
                    notices: &[Diagnostic { code: "00000", message: r#"start DROP INDEX"#, ..N }, Diagnostic { code: "00000", message: r#"dropped index public.e_a gone=t"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE CONCURRENTLY e;",
                    expected: Expected::Tag("REINDEX"),
                    notices: &[Diagnostic { code: "00000", message: r#"start REINDEX"#, ..N }, Diagnostic { code: "00000", message: r#"end REINDEX public.e_pkey valid=t"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_reindex_concurrently_keeps_expressions_as_written() {
    run_scripts(&[
        ScriptTest {
            name: "REINDEX CONCURRENTLY copies an index's expressions and predicate as the catalog holds them",
            set_up_script: &[
                "CREATE TABLE concur_exprs_tab (c1 INT, c2 BOOLEAN);",
                "INSERT INTO concur_exprs_tab (c1, c2) VALUES (1369652450, FALSE), (414515746, TRUE), (897778963, FALSE);",
                "CREATE UNIQUE INDEX concur_exprs_index_pred ON concur_exprs_tab (c1) WHERE (c1::text > 500000000::text COLLATE \"C\");",
                "CREATE UNIQUE INDEX concur_exprs_index_pred_2 ON concur_exprs_tab ((1 / c1)) WHERE ('-H') >= (c2::TEXT) COLLATE \"C\";",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "REINDEX TABLE CONCURRENTLY concur_exprs_tab;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('concur_exprs_index_pred'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T(r#"CREATE UNIQUE INDEX concur_exprs_index_pred ON public.concur_exprs_tab USING btree (c1) WHERE ((c1)::text > ((500000000)::text COLLATE "C"))"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('concur_exprs_index_pred_2'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T(r#"CREATE UNIQUE INDEX concur_exprs_index_pred_2 ON public.concur_exprs_tab USING btree (((1 / c1))) WHERE ('-H'::text >= ((c2)::text COLLATE "C"))"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE concur_exprs_tab ALTER c2 TYPE TEXT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('concur_exprs_index_pred'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T(r#"CREATE UNIQUE INDEX concur_exprs_index_pred ON public.concur_exprs_tab USING btree (c1) WHERE ((c1)::text > ((500000000)::text COLLATE "C"))"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('concur_exprs_index_pred_2'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T(r#"CREATE UNIQUE INDEX concur_exprs_index_pred_2 ON public.concur_exprs_tab USING btree (((1 / c1))) WHERE ('-H'::text >= (c2 COLLATE "C"))"#)],
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
fn test_reindex_concurrently_verbose_reports_primary_keys() {
    run_scripts(&[
        ScriptTest {
            name: "REINDEX CONCURRENTLY VERBOSE reports a primary key, which keys its table and has nothing to rebuild",
            set_up_script: &[
                "CREATE TABLE rv (id INT PRIMARY KEY, v INT);",
                "CREATE TABLE rw (id INT PRIMARY KEY, v INT);",
                "CREATE INDEX rw_v ON rw (v);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "REINDEX (VERBOSE) TABLE CONCURRENTLY rv;",
                    expected: Expected::Tag("REINDEX"),
                    notices: &[
                        Diagnostic { severity: "INFO", code: "00000", message: r#"index "public.rv_pkey" was reindexed"#, ..N },
                        Diagnostic { severity: "INFO", code: "00000", message: r#"table "public.rv" was reindexed"#, detail: "CPU: user: ", detail_contains: true, ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX (VERBOSE) INDEX CONCURRENTLY rv_pkey;",
                    expected: Expected::Tag("REINDEX"),
                    notices: &[
                        Diagnostic { severity: "INFO", code: "00000", message: r#"index "public.rv_pkey" was reindexed"#, detail: "CPU: user: ", detail_contains: true, ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX (VERBOSE) TABLE CONCURRENTLY rw;",
                    expected: Expected::Tag("REINDEX"),
                    notices: &[
                        Diagnostic { severity: "INFO", code: "00000", message: r#"index "public.rw_pkey" was reindexed"#, ..N },
                        Diagnostic { severity: "INFO", code: "00000", message: r#"index "public.rw_v" was reindexed"#, ..N },
                        Diagnostic { severity: "INFO", code: "00000", message: r#"table "public.rw" was reindexed"#, detail: "CPU: user: ", detail_contains: true, ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE CONCURRENTLY rv;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REINDEX SCHEMA CONCURRENTLY VERBOSE reports a primary key and nothing for a table without indexes",
            set_up_script: &[
                "CREATE SCHEMA rs;",
                "CREATE TABLE rs.noidx (v INT);",
                "CREATE TABLE rs.keyed (id INT PRIMARY KEY, v INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "REINDEX (VERBOSE) SCHEMA CONCURRENTLY rs;",
                    expected: Expected::Tag("REINDEX"),
                    notices: &[
                        Diagnostic { severity: "INFO", code: "00000", message: r#"index "rs.keyed_pkey" was reindexed"#, ..N },
                        Diagnostic { severity: "INFO", code: "00000", message: r#"table "rs.keyed" was reindexed"#, detail: "CPU: user: ", detail_contains: true, ..N },
                    ],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_writers_beside_a_concurrent_build() {
    run_scripts(&[
        ScriptTest {
            name: "a writer whose snapshot is older than a concurrent build changes rows the build indexed",
            set_up_script: &[
                "CREATE TABLE wt (id INT PRIMARY KEY, v INT);",
                "INSERT INTO wt SELECT i, i FROM generate_series(1, 1000) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"BEGIN ISOLATION LEVEL REPEATABLE READ;"#,
                    expected: Expected::Tag("BEGIN"),
                    client: "w",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM wt;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "w",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE INDEX CONCURRENTLY wt_v ON wt (v);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    client: "b",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM wt WHERE id = 5;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "w",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE wt SET v = v + 1000 WHERE id = 6;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "w",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE wt SET v = v + 1000 WHERE id = 7;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "w",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE wt SET v = v - 1000 WHERE id = 7;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "w",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMIT;"#,
                    expected: Expected::Tag("COMMIT"),
                    client: "w",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT indisvalid::text FROM pg_index WHERE indexrelid = 'wt_v'::regclass;"#,
                    expected: Expected::Rows {
                        columns: &[Column("indisvalid", TEXT)],
                        rows: &[
                            &[T("true")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM wt WHERE v >= 0;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("999")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM wt WHERE v = 5;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id FROM wt WHERE v = 1006;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id FROM wt WHERE v = 7;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("7")],
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
fn test_drop_index_concurrently_of_a_temporary_partitioned_index() {
    run_scripts(&[
        ScriptTest {
            name: "DROP INDEX CONCURRENTLY drops a temporary partitioned index, since a temporary relation is dropped without CONCURRENTLY",
            set_up_script: &[
                "CREATE TEMP TABLE idxpart_temp (a int) PARTITION BY RANGE (a);",
                "CREATE INDEX ON idxpart_temp (a);",
                "CREATE TEMP TABLE idxpart1_temp PARTITION OF idxpart_temp FOR VALUES FROM (0) TO (10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP INDEX idxpart1_temp_a_idx;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop index idxpart1_temp_a_idx because index idxpart_temp_a_idx requires it", hint: "You can drop index idxpart_temp_a_idx instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX CONCURRENTLY idxpart_temp_a_idx;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relkind FROM pg_class WHERE relname LIKE 'idxpart_temp%' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relkind", CHAR)],
                        rows: &[
                            &[T("idxpart_temp"), T("p")],
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
