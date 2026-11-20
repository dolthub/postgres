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
fn test_tablesample() {
    run_scripts(&[
        ScriptTest {
            name: "TABLESAMPLE's methods, estimates, and errors",
            set_up_script: &[
                r#"CREATE TABLE ts (id INTEGER PRIMARY KEY, v TEXT);"#,
                r#"INSERT INTO ts SELECT i, 'v' || i FROM generate_series(1, 1000) i;"#,
                r#"ANALYZE ts;"#,
                r#"CREATE VIEW tsv AS SELECT * FROM ts;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"EXPLAIN (COSTS OFF) SELECT * FROM ts TABLESAMPLE SYSTEM (50);"#,
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sample Scan on ts")],
                            &[T("  Sampling: system ('50'::real)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXPLAIN (COSTS OFF) SELECT * FROM ts TABLESAMPLE BERNOULLI (50) REPEATABLE (1) WHERE id > 5;"#,
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sample Scan on ts")],
                            &[T("  Sampling: bernoulli ('50'::real) REPEATABLE ('1'::double precision)")],
                            &[T("  Filter: (id > 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXPLAIN (COSTS OFF) SELECT * FROM ts t1 TABLESAMPLE SYSTEM (10), LATERAL (SELECT * FROM ts t2 TABLESAMPLE BERNOULLI (t1.id)) s WHERE t1.id < 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop")],
                            &[T("  ->  Sample Scan on ts t1")],
                            &[T("        Sampling: system ('10'::real)")],
                            &[T("        Filter: (id < 3)")],
                            &[T("  ->  Sample Scan on ts t2")],
                            &[T("        Sampling: bernoulli (t1.id)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM ts TABLESAMPLE BERNOULLI (0);"#,
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
                    query: r#"SELECT count(*) FROM ts TABLESAMPLE BERNOULLI (100);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM ts TABLESAMPLE SYSTEM (0);"#,
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
                    query: r#"SELECT count(*) FROM ts TABLESAMPLE SYSTEM (100);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) BETWEEN 300 AND 700 FROM ts TABLESAMPLE BERNOULLI (50) REPEATABLE (7);"#,
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
                    query: r#"SELECT (SELECT array_agg(id) FROM ts TABLESAMPLE BERNOULLI (5) REPEATABLE (3)) = (SELECT array_agg(id) FROM ts TABLESAMPLE BERNOULLI (5) REPEATABLE (3));"#,
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
                    query: r#"SELECT ctid IS NOT NULL FROM ts TABLESAMPLE SYSTEM (100) LIMIT 1;"#,
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
                    query: r#"SELECT * FROM ts TABLESAMPLE BERNOULLI (-1);"#,
                    expected: Expected::Error(Diagnostic { code: "2202H", message: "sample percentage must be between 0 and 100", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM ts TABLESAMPLE SYSTEM (101);"#,
                    expected: Expected::Error(Diagnostic { code: "2202H", message: "sample percentage must be between 0 and 100", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM ts TABLESAMPLE BERNOULLI (NULL);"#,
                    expected: Expected::Error(Diagnostic { code: "2202H", message: "TABLESAMPLE parameter cannot be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM ts TABLESAMPLE BERNOULLI (50) REPEATABLE (NULL);"#,
                    expected: Expected::Error(Diagnostic { code: "2202G", message: "TABLESAMPLE REPEATABLE parameter cannot be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM ts TABLESAMPLE foo (50);"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: "tablesample method foo does not exist", position: 30, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM ts TABLESAMPLE BERNOULLI (50, 2);"#,
                    expected: Expected::Error(Diagnostic { code: "2202H", message: "tablesample method bernoulli requires 1 argument, not 2", position: 30, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM tsv TABLESAMPLE BERNOULLI (50);"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "TABLESAMPLE clause can only be applied to tables and materialized views", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"PREPARE p AS SELECT count(*) FROM ts TABLESAMPLE BERNOULLI ($1) REPEATABLE (1);"#,
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXECUTE p(100);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXECUTE p(0);"#,
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
            name: "a REPEATABLE sample is the same while the rows are",
            set_up_script: &[
                r#"CREATE TABLE ts1 (id INTEGER PRIMARY KEY, v TEXT);"#,
                r#"INSERT INTO ts1 SELECT i, repeat('v', i % 50) FROM generate_series(1, 3000) i;"#,
                r#"CREATE TABLE ts2 (id INTEGER PRIMARY KEY, v TEXT);"#,
                r#"INSERT INTO ts2 SELECT i, repeat('v', i % 50) FROM generate_series(1, 1500) i;"#,
                r#"CREATE TABLE before_checkpoint (ids INTEGER[]);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO before_checkpoint SELECT array_agg(id ORDER BY id) FROM ts1 TABLESAMPLE SYSTEM (30) REPEATABLE (11);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CHECKPOINT;"#,
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT (SELECT array_agg(id ORDER BY id) FROM ts1 TABLESAMPLE SYSTEM (30) REPEATABLE (11)) = (SELECT ids FROM before_checkpoint);"#,
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
                    query: r#"INSERT INTO ts2 SELECT i, repeat('v', i % 50) FROM generate_series(1501, 3000) i;"#,
                    expected: Expected::Tag("INSERT 0 1500"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT (SELECT array_agg(id ORDER BY id) FROM ts1 TABLESAMPLE SYSTEM (30) REPEATABLE (11)) = (SELECT array_agg(id ORDER BY id) FROM ts2 TABLESAMPLE SYSTEM (30) REPEATABLE (11));"#,
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
                    query: r#"SELECT (SELECT array_agg(id ORDER BY id) FROM ts1 TABLESAMPLE BERNOULLI (30) REPEATABLE (11)) = (SELECT array_agg(id ORDER BY id) FROM ts2 TABLESAMPLE BERNOULLI (30) REPEATABLE (11));"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a serializable read through TABLESAMPLE takes part in write skew",
            set_up_script: &[
                "CREATE TABLE on_call (id INTEGER PRIMARY KEY, shift INTEGER);",
                "INSERT INTO on_call VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM on_call TABLESAMPLE SYSTEM (100) WHERE shift = 1",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM on_call TABLESAMPLE SYSTEM (100) WHERE shift = 2",
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
                    query: "/* client A */ INSERT INTO on_call VALUES (3, 2)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO on_call VALUES (4, 1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during commit attempt.", hint: "The transaction might succeed if retried.", ..E }),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a REPEATABLE sample is the same on a branch with the same rows",
            set_up_script: &[
                "CREATE TABLE ts3 (id INTEGER PRIMARY KEY, v TEXT);",
                "INSERT INTO ts3 SELECT i, repeat('v', i % 50) FROM generate_series(1, 3000) i;",
                "CREATE TABLE sample_on_main (ids INTEGER[]);",
                "INSERT INTO sample_on_main SELECT array_agg(id ORDER BY id) FROM ts3 TABLESAMPLE BERNOULLI (30) REPEATABLE (5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'sampled rows');",
                    expected: Expected::Rows { columns: &[Column("dolt_commit", TEXT)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'other');",
                    expected: Expected::Rows { columns: &[Column("dolt_checkout", RECORD)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT array_agg(id ORDER BY id) FROM ts3 TABLESAMPLE BERNOULLI (30) REPEATABLE (5)) = (SELECT ids FROM sample_on_main);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
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
fn test_tablesample_of_a_table_made_under_a_dropped_indexs_name() {
    run_scripts(&[
        ScriptTest {
            name: "TABLESAMPLE reads a table made in its transaction under the name of an index it dropped by the table's own keys",
            set_up_script: &[
                "CREATE TABLE ts_remade (a int4range, b int, c text);",
                "INSERT INTO ts_remade SELECT int4range(i, i + 5), i, 'name ' || i FROM generate_series(1, 300) i;",
                "CREATE INDEX ts_remade_i ON ts_remade (c);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX ts_remade_i;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_remade_i (k INT PRIMARY KEY, v TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ts_remade_i SELECT i, repeat('v', 50) FROM generate_series(1, 500) i;",
                    expected: Expected::Tag("INSERT 0 500"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ts_remade_i TABLESAMPLE SYSTEM (100) REPEATABLE (7);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ts_remade_i TABLESAMPLE BERNOULLI (100) REPEATABLE (7);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ts_remade_i TABLESAMPLE SYSTEM (100) REPEATABLE (7);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
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
fn test_tablesample_picks_the_blocks_and_rows_postgres_picks() {
    run_scripts(&[
        ScriptTest {
            name: "SYSTEM and BERNOULLI pick the rows Postgres' tablesample.sql picks, by ctid block and offset",
            set_up_script: &[
                "CREATE TABLE test_tablesample (id INT, name TEXT) WITH (fillfactor = 10);",
                "INSERT INTO test_tablesample SELECT i, repeat(i::TEXT, 200) FROM generate_series(0, 9) s(i);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT t.id FROM test_tablesample AS t TABLESAMPLE SYSTEM (50) REPEATABLE (0);"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (100.0/11) REPEATABLE (0);"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id FROM test_tablesample TABLESAMPLE BERNOULLI (50) REPEATABLE (0);"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id FROM test_tablesample TABLESAMPLE BERNOULLI (5.5) REPEATABLE (0);"#,
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
        ScriptTest {
            name: "BERNOULLI picks rows by their offsets in their blocks, as Postgres does, past the rows a delete removed",
            set_up_script: &[
                "CREATE TABLE tsk (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO tsk SELECT i, 'v' || i FROM generate_series(1, 1000) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT array_agg(id ORDER BY id) FROM tsk TABLESAMPLE BERNOULLI (2) REPEATABLE (3);"#,
                    expected: Expected::Rows {
                        columns: &[Column("array_agg", INT4_ARRAY)],
                        rows: &[
                            &[T("{31,47,256,384,477,527,659,719,769,899,911,976,982}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT array_agg(id ORDER BY id) FILTER (WHERE id % 50 = 0) FROM tsk TABLESAMPLE SYSTEM (40) REPEATABLE (5);"#,
                    expected: Expected::Rows {
                        columns: &[Column("array_agg", INT4_ARRAY)],
                        rows: &[
                            &[T("{50,100,150,200,600,650,700,750,800,850,900}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM tsk WHERE id % 3 = 0;"#,
                    expected: Expected::Tag("DELETE 333"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT array_agg(id ORDER BY id) FROM tsk TABLESAMPLE BERNOULLI (2) REPEATABLE (3);"#,
                    expected: Expected::Rows {
                        columns: &[Column("array_agg", INT4_ARRAY)],
                        rows: &[
                            &[T("{31,47,256,527,659,719,769,899,911,976,982}")],
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
