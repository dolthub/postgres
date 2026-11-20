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
fn test_vacuum() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM (PARALLEL) of temporary tables",
            set_up_script: &[
                "CREATE TEMPORARY TABLE vp_tmp (a int PRIMARY KEY, b int);",
                "CREATE INDEX vp_tmp_b ON vp_tmp (b);",
                "CREATE TEMPORARY TABLE vp_one (a int PRIMARY KEY);",
                "CREATE TEMPORARY TABLE vp_off (a int PRIMARY KEY, b int) WITH (vacuum_index_cleanup = off);",
                "CREATE INDEX vp_off_b ON vp_off (b);",
                "CREATE TABLE vp_perm (a int PRIMARY KEY, b int);",
                "CREATE INDEX vp_perm_b ON vp_perm (b);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 1, FULL FALSE) vp_tmp;",
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"disabling parallel option of vacuum on "vp_tmp" --- cannot vacuum temporary tables in parallel"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 0) vp_tmp;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM vp_tmp;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 2, INDEX_CLEANUP OFF) vp_tmp;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 2, INDEX_CLEANUP AUTO) vp_tmp;",
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"disabling parallel option of vacuum on "vp_tmp" --- cannot vacuum temporary tables in parallel"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 1) vp_one;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 1) vp_off;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 1, INDEX_CLEANUP ON) vp_off;",
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"disabling parallel option of vacuum on "vp_off" --- cannot vacuum temporary tables in parallel"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 1) vp_perm;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 1) vp_tmp, vp_off, vp_perm;",
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"disabling parallel option of vacuum on "vp_tmp" --- cannot vacuum temporary tables in parallel"#, ..E }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ANALYZE of a temporary table after another's",
            set_up_script: &[
                "CREATE TEMP TABLE x1 (a int, b int);",
                "INSERT INTO x1 SELECT g, g % 100 FROM generate_series(1, 1000) g;",
                "ANALYZE x1;",
                "DROP TABLE x1;",
                "CREATE TEMP TABLE skip_fetch (a INT, b INT) WITH (fillfactor=10);",
                "INSERT INTO skip_fetch SELECT i % 3, i FROM generate_series(0,30) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE skip_fetch;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT staattnum, stadistinct FROM pg_statistic WHERE starelid = 'skip_fetch'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stadistinct", FLOAT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("2"), T("-1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ANALYZE of a system catalog after another's",
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE pg_class;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE pg_type;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT histogram_bounds::text FROM pg_stats WHERE tablename = 'pg_type' AND attname = 'oid') = (SELECT histogram_bounds::text FROM pg_stats WHERE tablename = 'pg_class' AND attname = 'oid') AS same;",
                    expected: Expected::Rows {
                        columns: &[Column("same", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT (histogram_bounds::text::oid[])[1] FROM pg_stats WHERE tablename = 'pg_type' AND attname = 'oid') IN (SELECT oid FROM pg_type) AS first_is_type;",
                    expected: Expected::Rows {
                        columns: &[Column("first_is_type", BOOL)],
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
fn test_vacuum_truncate() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM's TRUNCATE option, the vacuum_truncate setting, and the table's option, Postgres' vacuum.sql",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE vac_truncate_test (i INT NOT NULL, j TEXT) WITH (vacuum_truncate = true, autovacuum_enabled = false);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vac_truncate_test VALUES (1, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (TRUNCATE FALSE, DISABLE_PAGE_SKIPPING) vac_truncate_test;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('vac_truncate_test') > 0;",
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
                    query: "SET vacuum_truncate = false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (DISABLE_PAGE_SKIPPING) vac_truncate_test;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('vac_truncate_test') = 0;",
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
                    query: "VACUUM (TRUNCATE FALSE, FULL TRUE) vac_truncate_test;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE vac_truncate_test RESET (vacuum_truncate);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vac_truncate_test VALUES (1, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (DISABLE_PAGE_SKIPPING) vac_truncate_test;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('vac_truncate_test') > 0;",
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
                    query: "RESET vacuum_truncate;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (TRUNCATE FALSE, DISABLE_PAGE_SKIPPING) vac_truncate_test;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('vac_truncate_test') > 0;",
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
                    query: "VACUUM (DISABLE_PAGE_SKIPPING) vac_truncate_test;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('vac_truncate_test') = 0;",
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
            name: "a table's vacuum_truncate option, Postgres' reloptions.sql",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE reloptions_test (i INT NOT NULL, j TEXT) WITH (vacuum_truncate = false, autovacuum_enabled = false);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO reloptions_test VALUES (1, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) reloptions_test;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('reloptions_test') > 0;",
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
                    query: "ALTER TABLE reloptions_test RESET (vacuum_truncate);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO reloptions_test VALUES (1, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) reloptions_test;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('reloptions_test') = 0;",
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
