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

//! Dolt's integration-tests/bats/1pk5col-ints.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_1pk5col_ints() {
    run_scripts(&[
        ScriptTest {
            name: "1pk5col-ints: empty table",
            intent: "A new empty table is listed, has no rows, shows as an added table in the diff, and is untracked.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls --verbose prints each table's row count, which count(*) gives")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.test"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: create a table, dolt add, dolt reset, and dolt commit",
            intent: "dolt_add stages a new table, dolt_reset unstages it, and dolt_commit commits it into the log.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'test commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("test commit")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: add a row to a created table using dolt table put-row",
            intent: "A row inserted after the commit shows as added in the working diff.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'create table');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c5, from_pk, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT8), Column("to_c1", INT8), Column("to_c5", INT8), Column("from_pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("1"), T("5"), Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql all manner of inserts",
            intent: "Inserts with all or some columns write rows, and inserts missing the key, naming an unknown column, or repeating a key fail.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,6,6,6,6,6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("6"), T("6"), T("6"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (1,7,7,7,7,7),(2,8,8,8,8,8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("6"), T("6"), T("6"), T("6"), T("6")],
                            &[T("1"), T("7"), T("7"), T("7"), T("7"), T("7")],
                            &[T("2"), T("8"), T("8"), T("8"), T("8"), T("8")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c3,c5) VALUES (3,9,9,9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("6"), T("6"), T("6"), T("6"), T("6")],
                            &[T("1"), T("7"), T("7"), T("7"), T("7"), T("7")],
                            &[T("2"), T("8"), T("8"), T("8"), T("8"), T("8")],
                            &[T("3"), T("9"), Null, T("9"), Null, T("9")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (c1,c3,c5) VALUES (50,55,60);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"pk\" of relation \"test\" violates not-null constraint", detail: "Failing row contains (null, 50, null, 55, null, 60).", schema: "public", table: "test", column: "pk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5,c6) VALUES (10,1,1,1,1,1,1);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c6\" of relation \"test\" does not exist", position: 37, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,6,6,6,6,6);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(0) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql insert same column twice",
            intent: "An insert naming a column twice fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c1) VALUES (3,1,2);",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"c1\" specified more than once", position: 25, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql insert no columns specified",
            intent: "An insert without a column list fills the columns in order, and one with too many values fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0,0,0,0,0,0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4,1,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql with insert ignore",
            intent: "An insert that skips conflicting rows still writes the others.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,6,6,6,6,6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,6,6,6,6,6),(11,111,111,111,111,111) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("6"), T("6"), T("6"), T("6"), T("6")],
                            &[T("11"), T("111"), T("111"), T("111"), T("111"), T("111")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql replace into",
            intent: "An upsert replaces an existing row and inserts a new one.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,6,6,6,6,6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,7,7,7,7,7),(1,8,8,8,8,8) ON CONFLICT (\"pk\") DO UPDATE SET \"c1\" = EXCLUDED.\"c1\", \"c2\" = EXCLUDED.\"c2\", \"c3\" = EXCLUDED.\"c3\", \"c4\" = EXCLUDED.\"c4\", \"c5\" = EXCLUDED.\"c5\";",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("7"), T("7"), T("7"), T("7"), T("7")],
                            &[T("1"), T("8"), T("8"), T("8"), T("8"), T("8")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql insert and dolt sql select",
            intent: "Inserted rows are read back by key, column, LIMIT, and comparisons, and an unknown column fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,1,2,3,4,5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (101,102,103,104,105,106),(1,6,7,8,9,10);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("101"), T("102"), T("103"), T("104"), T("105"), T("106")],
                            &[T("1"), T("6"), T("7"), T("8"), T("9"), T("10")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("1"), T("6"), T("7"), T("8"), T("9"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c5 FROM test WHERE pk=1;",
                    expected: Expected::Rows {
                        columns: &[Column("c5", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE c2 > 7;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("101"), T("102"), T("103"), T("104"), T("105"), T("106")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE c2 >= 7;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("101"), T("102"), T("103"), T("104"), T("105"), T("106")],
                            &[T("1"), T("6"), T("7"), T("8"), T("9"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE c2 <> 7;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("101"), T("102"), T("103"), T("104"), T("105"), T("106")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE c2 > 3 AND c1 < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("1"), T("6"), T("7"), T("8"), T("9"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c10 FROM test WHERE pk=1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c10\" does not exist", hint: "Perhaps you meant to reference the column \"test.c1\".", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE c2=147;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql select as",
            intent: "Column aliases name the result columns.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,1,2,3,4,5),(1,11,12,13,14,15),(2,21,22,23,24,25);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 AS column1, c2 AS column2 FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT8), Column("column2", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("11"), T("12")],
                            &[T("21"), T("22")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 AS column1 FROM test WHERE c1=1;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql select with inverted where clause",
            intent: "A comparison with the constant on the left filters rows.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,1,2,3,4,5),(1,11,12,13,14,15),(2,21,22,23,24,25);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE 5 > c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql update queries",
            intent: "Updates change the matching rows, and an unknown column or a value of the wrong type fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,1,2,3,4,5),(1,11,12,13,14,15),(2,21,22,23,24,25);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c1=6,c2=7,c3=8,c4=9,c5=10 WHERE pk=0;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("6"), T("7"), T("8"), T("9"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (4,11,12,13,14,15);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c2=11,c3=11,c4=11,c5=11 WHERE c1=11;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("2"), T("21"), T("22"), T("23"), T("24"), T("25")],
                            &[T("0"), T("6"), T("7"), T("8"), T("9"), T("10")],
                            &[T("1"), T("11"), T("11"), T("11"), T("11"), T("11")],
                            &[T("4"), T("11"), T("11"), T("11"), T("11"), T("11")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c2=50,c3=50,c4=50,c5=50 WHERE c1=50;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("2"), T("21"), T("22"), T("23"), T("24"), T("25")],
                            &[T("0"), T("6"), T("7"), T("8"), T("9"), T("10")],
                            &[T("1"), T("11"), T("11"), T("11"), T("11"), T("11")],
                            &[T("4"), T("11"), T("11"), T("11"), T("11"), T("11")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c12=11 WHERE pk=0;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c12\" of relation \"test\" does not exist", position: 17, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c1='foo' WHERE pk=0;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type bigint: \"foo\"", position: 20, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c1=100,c2=100,c3=100,c4=100,c5=100 WHERE pk>0;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("6"), T("7"), T("8"), T("9"), T("10")],
                            &[T("1"), T("100"), T("100"), T("100"), T("100"), T("100")],
                            &[T("2"), T("100"), T("100"), T("100"), T("100"), T("100")],
                            &[T("4"), T("100"), T("100"), T("100"), T("100"), T("100")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt sql delete queries",
            intent: "Deletes remove the matching rows, and an unknown column fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,1,2,3,4,5),(1,11,12,13,14,15),(2,21,22,23,24,25);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk=2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,1,2,3,4,5),(1,11,12,13,14,15),(2,21,22,23,24,25);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk>0;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE c1=1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (0,1,2,3,4,5),(1,11,12,13,14,15),(2,21,22,23,24,25);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE c10=1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c10\" does not exist", hint: "Perhaps you meant to reference the column \"test.c1\".", position: 24, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE c1='foo';",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type bigint: \"foo\"", position: 27, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt checkout to put a table back to its checked in state",
            intent: "dolt_checkout of a table discards its working changes.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Added table and test row');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 10) ON CONFLICT (\"pk\") DO UPDATE SET \"c1\" = EXCLUDED.\"c1\", \"c2\" = EXCLUDED.\"c2\", \"c3\" = EXCLUDED.\"c3\", \"c4\" = EXCLUDED.\"c4\", \"c5\" = EXCLUDED.\"c5\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('test');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt checkout branch and table name collision",
            intent: "dolt_checkout of a name that is both a branch and a table switches to the branch.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('test');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'test'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: make a change on a different branch, commit, and merge to main",
            intent: "A commit on a branch merges into main as a fast-forward.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'create table');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's SQL dolt_checkout leaves the working changes on the branch it leaves, where the CLI's dolt checkout carries them, so the port commits the table before branching")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('test-branch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('test-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'test-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added test row');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added test row")],
                            &[T("create table")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: create a branch off an older commit than HEAD",
            intent: "dolt_checkout -b at HEAD^ makes a branch whose log ends at that commit.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'first commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added test row');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'older-branch', 'HEAD^');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'older-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("older-branch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: delete an unmerged branch",
            intent: "dolt_branch -d refuses the checked out branch and an unmerged one, and -f deletes it.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'create table');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's SQL dolt_checkout leaves the working changes on the branch it leaves, where the CLI's dolt checkout carries them, so the port commits the table before branching")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'test-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added test row');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'test-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot delete checked out branch 'test-branch'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'test-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch 'test-branch' is not fully merged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', '-f', 'test-branch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: put a row that violates the schema",
            intent: "An insert of a value of the wrong type fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 'foo');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type bigint: \"foo\"", position: 41, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: put a row that has a column not in the schema",
            intent: "An insert with more values than columns fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5, 10);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT has more expressions than target columns", position: 44, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: import data from a csv file after table created",
            intent: "Loading a CSV file into a created table writes its rows.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file")),
                    copy_from_stdin_file: "bats-1pk5col-ints.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("1"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: import data from a csv file with a bad line",
            intent: "A file with a short line fails to load.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "missing data for column \"c1\"", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file")),
                    copy_from_stdin_file: "bats-1pk5col-ints-bad-line.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("COPY loads all of a file's rows or none, where dolt table import keeps the rows before the bad line")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: import data from a csv file with a bad header",
            intent: "A file whose header has an empty column name fails to load.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER MATCH);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "column name mismatch in header line field 1: got null value (\"\"), expected \"pk\"", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN with HEADER MATCH checks the file's header")),
                    copy_from_stdin_file: "bats-1pk5col-ints-bad-header-1.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER MATCH);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "column name mismatch in header line field 3: got \" \", expected \"c2\"", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN with HEADER MATCH checks the file's header")),
                    copy_from_stdin_file: "bats-1pk5col-ints-bad-header-2.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER MATCH);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "column name mismatch in header line field 3: got \"\", expected \"c2\"", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN with HEADER MATCH checks the file's header")),
                    copy_from_stdin_file: "bats-1pk5col-ints-bad-header-3.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER MATCH);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "column name mismatch in header line field 3: got \" \", expected \"c2\"", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN with HEADER MATCH checks the file's header")),
                    copy_from_stdin_file: "bats-1pk5col-ints-bad-header-4.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: import data from a psv file after table created",
            intent: "Loading a pipe-separated file into a created table writes its rows.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER, DELIMITER '|');",
                    expected: Expected::Tag("COPY 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file")),
                    copy_from_stdin_file: "bats-1pk5col-ints.psv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("1"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: overwrite a row. make sure it updates not inserts",
            intent: "An upsert of an existing key changes that row without adding one.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file")),
                    copy_from_stdin_file: "bats-1pk5col-ints.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2, 4, 6, 8, 10) ON CONFLICT (\"pk\") DO UPDATE SET \"c1\" = EXCLUDED.\"c1\", \"c2\" = EXCLUDED.\"c2\", \"c3\" = EXCLUDED.\"c3\", \"c4\" = EXCLUDED.\"c4\", \"c5\" = EXCLUDED.\"c5\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("1"), T("2"), T("4"), T("6"), T("8"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt schema show",
            intent: "The table's columns, types, NOT NULL, and primary key are as created.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("t"), T("t")],
                            &[T("c1"), T("bigint"), T("f"), T("f")],
                            &[T("c2"), T("bigint"), T("f"), T("f")],
                            &[T("c3"), T("bigint"), T("f"), T("f")],
                            &[T("c4"), T("bigint"), T("f"), T("f")],
                            &[T("c5"), T("bigint"), T("f"), T("f")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: dolt schema show on non existent table",
            intent: "Looking up a missing table fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'foo'::regclass;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"foo\" does not exist", position: 8, ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so a cast to regclass looks the table up")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: rm a staged but uncommitted table",
            intent: "Staging the drop of a staged new table leaves nothing to commit.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::Differs("dolt table rm is the CLI's, so DROP TABLE removes the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: create and view a table with NULL values",
            intent: "Columns left out of an insert read as NULL.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), Null, Null, Null, Null, Null],
                            &[T("1"), Null, Null, Null, Null, Null],
                            &[T("2"), Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), Null, Null, Null, Null, Null],
                            &[T("1"), Null, Null, Null, Null, Null],
                            &[T("2"), Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "1pk5col-ints: using dolt sql to select rows with NULL values",
            intent: "IS NULL selects the rows whose column is NULL.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 0, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE c1 IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), Null, Null, Null, Null, Null],
                            &[T("1"), Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"1pk5col-ints: generate a merge conflict and resolve with ours"#,
            intent: r#"Resolving a merge's row conflict with ours keeps our row, clears the conflict, and commits a merge with both lines of history."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test table');"#,
                r#"SELECT dolt_branch('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test row');"#,
                r#"SELECT * FROM dolt_checkout('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 6);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added conflicting test row');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = on;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('test-branch', '--no-commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, whose CONFLICT (content) line is dolt_merge's conflicts column, and the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_c5, their_pk, their_c5 FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT8), Column("our_c5", INT8), Column("their_pk", INT8), Column("their_c5", INT8)],
                        rows: &[
                            &[T("0"), T("5"), T("0"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_conflicts;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'merged and resolved conflict');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('added test row', 'added conflicting test row', 'merged and resolved conflict') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added conflicting test row")],
                            &[T("added test row")],
                            &[T("merged and resolved conflict")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log's Merge: line marks a commit with two parents, which dolt_commit_ancestors lists")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"1pk5col-ints: generate a merge conflict and resolve with ours using stored procedure"#,
            intent: r#"Resolving a merge's row conflict with ours keeps our row, clears the conflict, and commits a merge with both lines of history."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test table');"#,
                r#"SELECT dolt_branch('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test row');"#,
                r#"SELECT * FROM dolt_checkout('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 6);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added conflicting test row');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = on;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('test-branch', '--no-commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, whose CONFLICT (content) line is dolt_merge's conflicts column, and the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_c5, their_pk, their_c5 FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT8), Column("our_c5", INT8), Column("their_pk", INT8), Column("their_c5", INT8)],
                        rows: &[
                            &[T("0"), T("5"), T("0"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_conflicts;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'merged and resolved conflict');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('added test row', 'added conflicting test row', 'merged and resolved conflict') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added conflicting test row")],
                            &[T("added test row")],
                            &[T("merged and resolved conflict")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log's Merge: line marks a commit with two parents, which dolt_commit_ancestors lists")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"1pk5col-ints: generate a merge conflict and try to roll back using dolt merge --abort"#,
            intent: r#"Aborting a merge with conflicts restores the branch's rows and a clean working set."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test table');"#,
                r#"SELECT dolt_branch('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test row');"#,
                r#"SELECT * FROM dolt_checkout('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 6);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added conflicting test row');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = on;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('test-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, whose CONFLICT (content) line is dolt_merge's conflicts column, and the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('--abort');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"1pk5col-ints: generate a merge conflict and resolve with theirs"#,
            intent: r#"Resolving a merge's row conflict with theirs keeps their row, clears the conflict, and commits a merge with both lines of history."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test table');"#,
                r#"SELECT dolt_branch('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test row');"#,
                r#"SELECT * FROM dolt_checkout('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 6);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added conflicting test row');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = on;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('test-branch', '--no-commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, whose CONFLICT (content) line is dolt_merge's conflicts column, and the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_c5, their_pk, their_c5 FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT8), Column("our_c5", INT8), Column("their_pk", INT8), Column("their_c5", INT8)],
                        rows: &[
                            &[T("0"), T("5"), T("0"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_conflicts;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'merged and resolved conflict');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('added test row', 'added conflicting test row', 'merged and resolved conflict') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added conflicting test row")],
                            &[T("added test row")],
                            &[T("merged and resolved conflict")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log's Merge: line marks a commit with two parents, which dolt_commit_ancestors lists")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"1pk5col-ints: generate a merge conflict and resolve with theirs using stored procedure"#,
            intent: r#"Resolving a merge's row conflict with theirs keeps their row, clears the conflict, and commits a merge with both lines of history."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test table');"#,
                r#"SELECT dolt_branch('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test row');"#,
                r#"SELECT * FROM dolt_checkout('test-branch');"#,
                r#"INSERT INTO test VALUES (0, 1, 2, 3, 4, 6);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added conflicting test row');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = on;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('test-branch', '--no-commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, whose CONFLICT (content) line is dolt_merge's conflicts column, and the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_c5, their_pk, their_c5 FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT8), Column("our_c5", INT8), Column("their_pk", INT8), Column("their_c5", INT8)],
                        rows: &[
                            &[T("0"), T("5"), T("0"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_conflicts;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'merged and resolved conflict');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('added test row', 'added conflicting test row', 'merged and resolved conflict') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added conflicting test row")],
                            &[T("added test row")],
                            &[T("merged and resolved conflict")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log's Merge: line marks a commit with two parents, which dolt_commit_ancestors lists")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
