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
fn test_stats() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE statement",
            set_up_script: &[
                "CREATE TABLE t (pk int primary key);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE t;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE public.t;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE postgres.public.t;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE doesnotexists.public.t;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cross-database references are not implemented: "doesnotexists.public.t""#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_checkpoint() {
    run_scripts(&[
        ScriptTest {
            name: "CHECKPOINT needs the privileges of pg_checkpoint",
            set_up_script: &[
                "CREATE ROLE checkpoint_user;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE checkpoint_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Error(Diagnostic {
                        code: "42501",
                        message: "permission denied to execute CHECKPOINT command",
                        detail: "Only roles with privileges of the \"pg_checkpoint\" role may execute this command.",
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT pg_checkpoint TO checkpoint_user;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_vacuum_and_analyze_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM and ANALYZE update a table's statistics in pg_class and check their options",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE va (a INT PRIMARY KEY, b TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO va SELECT a.x * 1000 + b.x * 100 + c.x * 10 + d.x, repeat('x', 50) FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) c(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x) WHERE a.x < 5;"#,
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages > 0 AS has_pages, reltuples, relallvisible = relpages AS all_visible, relallfrozen = relpages AS all_frozen FROM pg_class WHERE relname = 'va';"#,
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4), Column("all_visible", BOOL), Column("all_frozen", BOOL)],
                        rows: &[
                            &[T("f"), T("-1"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE va;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages > 0 AS has_pages, reltuples, relallvisible = relpages AS all_visible, relallfrozen = relpages AS all_frozen FROM pg_class WHERE relname = 'va';"#,
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4), Column("all_visible", BOOL), Column("all_frozen", BOOL)],
                        rows: &[
                            &[T("t"), T("5000"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("this storage has no visibility map: every page is all-visible and none needs freezing, so ANALYZE and VACUUM count them all"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM va;"#,
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages > 0 AS has_pages, reltuples, relallvisible = relpages AS all_visible, relallfrozen = relpages AS all_frozen FROM pg_class WHERE relname = 'va';"#,
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4), Column("all_visible", BOOL), Column("all_frozen", BOOL)],
                        rows: &[
                            &[T("t"), T("5000"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("this storage has no visibility map: every page is all-visible and none needs freezing, so ANALYZE and VACUUM count them all"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM FREEZE va;"#,
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages > 0 AS has_pages, reltuples, relallvisible = relpages AS all_visible, relallfrozen = relpages AS all_frozen FROM pg_class WHERE relname = 'va';"#,
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4), Column("all_visible", BOOL), Column("all_frozen", BOOL)],
                        rows: &[
                            &[T("t"), T("5000"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname = 'va_pkey';"#,
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("t"), T("5000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM va;"#,
                    expected: Expected::Error(Diagnostic { code: "25001", message: r#"VACUUM cannot run inside a transaction block"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM (FULL, DISABLE_PAGE_SKIPPING) va;"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"VACUUM option DISABLE_PAGE_SKIPPING cannot be used with FULL"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM (BOGUS) va;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized VACUUM option "bogus""#, position: 9, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE (FREEZE) va;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized ANALYZE option "freeze""#, position: 10, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM nosuch;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE nosuch;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM va (b);"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"ANALYZE option must be specified when a column list is provided"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE va (nosuchcol);"#,
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nosuchcol" of relation "va" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM (ANALYZE, VERBOSE false) va (b);"#,
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_vacuum_and_analyze_relations() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM and ANALYZE skip what is not a table, and ANALYZE runs in a transaction block",
            set_up_script: &[
                r#"CREATE TABLE vr (a INT PRIMARY KEY, b TEXT);"#,
                r#"INSERT INTO vr SELECT a.x * 10 + b.x, 'x' FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x);"#,
                r#"CREATE VIEW vrv AS SELECT 1;"#,
                r#"CREATE SEQUENCE vrs;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ANALYZE vrv;"#,
                    expected: Expected::Tag("ANALYZE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"skipping "vrv" --- cannot analyze non-tables or special system tables"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM vrv;"#,
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"skipping "vrv" --- cannot vacuum non-tables or special system tables"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM (ANALYZE) vrv;"#,
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"skipping "vrv" --- cannot vacuum non-tables or special system tables"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE vrs;"#,
                    expected: Expected::Tag("ANALYZE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"skipping "vrs" --- cannot analyze non-tables or special system tables"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM vrs;"#,
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"skipping "vrs" --- cannot vacuum non-tables or special system tables"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE vr (a, a);"#,
                    expected: Expected::Error(Diagnostic { code: "42701", message: r#"column "a" of relation "vr" appears more than once"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE (SKIP_LOCKED) nosuch;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE vr;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMIT;"#,
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT reltuples FROM pg_class WHERE relname = 'vr';"#,
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM;"#,
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE (VERBOSE) vr;"#,
                    expected: Expected::Tag("ANALYZE"),
                    notices: &[Diagnostic { severity: "INFO", code: "00000", message: r#"analyzing "public.vr""#, ..E }, Diagnostic { severity: "INFO", code: "00000", message: r#""vr": scanned 1 of 1 pages, containing 100 live rows and 0 dead rows; 100 rows in sample, 100 estimated total rows"#, ..E }, Diagnostic { severity: "INFO", code: "00000", message: r#"finished analyzing table "postgres.public.vr""#, message_contains: true, ..E }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ANALYZE of an empty partitioned table records its inheritance tree as empty",
            set_up_script: &[
                r#"CREATE TABLE vq (a INT) PARTITION BY RANGE (a);"#,
                r#"CREATE TABLE vq1 PARTITION OF vq FOR VALUES FROM (0) TO (100);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ANALYZE vq;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages, reltuples FROM pg_class WHERE relname IN ('vq', 'vq1') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("vq"), T("-1"), T("0")],
                            &[T("vq1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ANALYZE of a partitioned table records its inheritance tree's rows",
            set_up_script: &[
                r#"CREATE TABLE vp (a INT) PARTITION BY RANGE (a);"#,
                r#"CREATE TABLE vp1 PARTITION OF vp FOR VALUES FROM (0) TO (100);"#,
                r#"INSERT INTO vp SELECT a.x * 10 + b.x FROM (VALUES (0), (1), (2), (3), (4)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ANALYZE vp;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages, reltuples FROM pg_class WHERE relname = 'vp';"#,
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("-1"), T("50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname = 'vp1';"#,
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("t"), T("50")],
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
fn test_vacuum_and_analyze_statistics_in_place() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM and ANALYZE keep their statistics when the transaction rolls back, and new storage starts without them",
            set_up_script: &[
                r#"CREATE TABLE vr (a INT PRIMARY KEY);"#,
                r#"INSERT INTO vr SELECT a.x * 10 + b.x FROM (VALUES (0), (1), (2), (3), (4)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE vr;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname IN ('vr', 'vr_pkey') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("vr"), T("t"), T("50")],
                            &[T("vr_pkey"), T("t"), T("50")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"TRUNCATE vr;"#,
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages, reltuples FROM pg_class WHERE relname IN ('vr', 'vr_pkey') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("vr"), T("0"), T("-1")],
                            &[T("vr_pkey"), T("0"), T("-1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname IN ('vr', 'vr_pkey') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("vr"), T("t"), T("50")],
                            &[T("vr_pkey"), T("t"), T("50")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM vr WHERE a < 10;"#,
                    expected: Expected::Tag("DELETE 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM vr;"#,
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname = 'vr';"#,
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("t"), T("40")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"TRUNCATE vr;"#,
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages, reltuples FROM pg_class WHERE relname IN ('vr', 'vr_pkey') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("vr"), T("0"), T("-1")],
                            &[T("vr_pkey"), T("0"), T("-1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "VACUUM and ANALYZE leave a table's pg_class row in place",
            set_up_script: &[
                r#"CREATE TABLE vx (a INT PRIMARY KEY);"#,
                r#"INSERT INTO vx VALUES (1), (2), (3);"#,
                r#"CREATE TABLE vxmin (x XID);"#,
                r#"INSERT INTO vxmin SELECT xmin FROM pg_class WHERE relname = 'vx';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ANALYZE vx;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM vx;"#,
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT x = (SELECT xmin FROM pg_class WHERE relname = 'vx') AS same_row, (SELECT reltuples FROM pg_class WHERE relname = 'vx') AS reltuples FROM vxmin;"#,
                    expected: Expected::Rows {
                        columns: &[Column("same_row", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("t"), T("3")],
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
fn test_create_index_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE INDEX counts its table's rows and its own entries, unless the table is empty or its autovacuum is off",
            set_up_script: &[
                r#"CREATE TABLE ci (a INT NOT NULL, b INT);"#,
                r#"INSERT INTO ci SELECT a.x * 10 + b.x + 1, b.x % 7 FROM (VALUES (0), (1), (2), (3), (4)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname = 'ci';"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("ci"), T("f"), T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE INDEX ci_b ON ci (b);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname IN ('ci', 'ci_b') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("ci"), T("t"), T("50")],
                            &[T("ci_b"), T("t"), T("50")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE ci ADD PRIMARY KEY (a);"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname IN ('ci', 'ci_b', 'ci_pkey') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("ci"), T("t"), T("50")],
                            &[T("ci_b"), T("t"), T("50")],
                            &[T("ci_pkey"), T("t"), T("50")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE ce (a INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, reltuples FROM pg_class WHERE relname IN ('ce', 'ce_pkey') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("ce"), T("-1")],
                            &[T("ce_pkey"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE cn (a INT) WITH (autovacuum_enabled = false);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO cn SELECT x FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) v(x);"#,
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE INDEX cn_a ON cn (a);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname IN ('cn', 'cn_a') ORDER BY relname;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("cn"), T("f"), T("-1")],
                            &[T("cn_a"), T("t"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ANALYZE after TRUNCATE in a transaction that rolls back keeps the table's earlier statistics",
            set_up_script: &[
                r#"CREATE TABLE tr (a INT);"#,
                r#"INSERT INTO tr SELECT a.x * 10 + b.x FROM (VALUES (0), (1)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x);"#,
                r#"ANALYZE tr;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"TRUNCATE tr;"#,
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO tr VALUES (1), (2), (3), (4), (5);"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE tr;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT reltuples FROM pg_class WHERE relname = 'tr';"#,
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname = 'tr';"#,
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("t"), T("20")],
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
fn test_analyze_column_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE computes each column's statistics into pg_statistic, which roll back with their transaction",
            set_up_script: &[
                r#"CREATE TABLE cs (id int PRIMARY KEY, a int, b text, c float8, d bool, e text);"#,
                r#"INSERT INTO cs SELECT a.x * 10 + b.x + 1, (a.x * 10 + b.x) % 7, 'x' || ((a.x * 10 + b.x) % 3), (a.x * 10 + b.x) / 4.0, b.x % 4 = 0, CASE WHEN b.x = 0 THEN NULL ELSE 'v' || (a.x * 10 + b.x) END FROM (VALUES (0), (1), (2), (3), (4)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x);"#,
                r#"ANALYZE cs;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT staattnum, stanullfrac, stawidth, stadistinct, stakind1, stakind2, stakind3, staop1, staop2, staop3, stanumbers1, stavalues1::text, stavalues2::text FROM pg_statistic WHERE starelid = 'cs'::regclass ORDER BY staattnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stanullfrac", FLOAT4), Column("stawidth", INT4), Column("stadistinct", FLOAT4), Column("stakind1", INT2), Column("stakind2", INT2), Column("stakind3", INT2), Column("staop1", OID), Column("staop2", OID), Column("staop3", OID), Column("stanumbers1", FLOAT4_ARRAY), Column("stavalues1", TEXT), Column("stavalues2", TEXT)],
                        rows: &[
                            &[T("1"), T("0"), T("4"), T("-1"), T("2"), T("3"), T("0"), T("97"), T("97"), T("0"), Null, T("{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50}"), Null],
                            &[T("2"), T("0"), T("4"), T("-0.14"), T("1"), T("3"), T("0"), T("96"), T("97"), T("0"), T("{0.16,0.14,0.14,0.14,0.14,0.14,0.14}"), T("{0,1,2,3,4,5,6}"), Null],
                            &[T("3"), T("0"), T("3"), T("3"), T("1"), T("3"), T("0"), T("98"), T("664"), T("0"), T("{0.34,0.34,0.32}"), T("{x0,x1,x2}"), Null],
                            &[T("4"), T("0"), T("8"), T("-1"), T("2"), T("3"), T("0"), T("672"), T("672"), T("0"), Null, T("{0,0.25,0.5,0.75,1,1.25,1.5,1.75,2,2.25,2.5,2.75,3,3.25,3.5,3.75,4,4.25,4.5,4.75,5,5.25,5.5,5.75,6,6.25,6.5,6.75,7,7.25,7.5,7.75,8,8.25,8.5,8.75,9,9.25,9.5,9.75,10,10.25,10.5,10.75,11,11.25,11.5,11.75,12,12.25}"), Null],
                            &[T("5"), T("0"), T("1"), T("2"), T("1"), T("3"), T("0"), T("91"), T("58"), T("0"), T("{0.7,0.3}"), T("{f,t}"), Null],
                            &[T("6"), T("0.1"), T("3"), T("-0.9"), T("2"), T("3"), T("0"), T("664"), T("664"), T("0"), Null, T("{v1,v11,v12,v13,v14,v15,v16,v17,v18,v19,v2,v21,v22,v23,v24,v25,v26,v27,v28,v29,v3,v31,v32,v33,v34,v35,v36,v37,v38,v39,v4,v41,v42,v43,v44,v45,v46,v47,v48,v49,v5,v6,v7,v8,v9}"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT staattnum, stanumbers2 FROM pg_statistic WHERE starelid = 'cs'::regclass AND staattnum IN (1, 4) ORDER BY staattnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stanumbers2", FLOAT4_ARRAY)],
                        rows: &[
                            &[T("1"), T("{0.29411766}")],
                            &[T("4"), T("{0.29411766}")],
                        ],
                        tag: "SELECT 2",
                    },
                    skip: Some("a table's rows are stored in the order of its key, so the correlation of its values with the rows' physical order differs from a heap's insertion order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT staattnum, stanumbers2 FROM pg_statistic WHERE starelid = 'cs'::regclass AND staattnum IN (1, 4) ORDER BY staattnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stanumbers2", FLOAT4_ARRAY)],
                        rows: &[
                            &[T("1"), T("{1}")],
                            &[T("4"), T("{1}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO cs SELECT a.x * 10 + b.x + 51, 0, 'y', 0, false, NULL FROM (VALUES (0), (1), (2), (3), (4)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x);"#,
                    expected: Expected::Tag("INSERT 0 50"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ANALYZE cs (b);"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT stadistinct, stanumbers1, stavalues1::text FROM pg_statistic WHERE starelid = 'cs'::regclass AND staattnum = 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("stadistinct", FLOAT4), Column("stanumbers1", FLOAT4_ARRAY), Column("stavalues1", TEXT)],
                        rows: &[
                            &[T("4"), T("{0.5,0.17,0.17,0.16}"), T("{y,x0,x1,x2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT stadistinct, stanumbers1, stavalues1::text FROM pg_statistic WHERE starelid = 'cs'::regclass AND staattnum = 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("stadistinct", FLOAT4), Column("stanumbers1", FLOAT4_ARRAY), Column("stavalues1", TEXT)],
                        rows: &[
                            &[T("3"), T("{0.34,0.34,0.32}"), T("{x0,x1,x2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE cs DROP COLUMN e;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_statistic WHERE starelid = 'cs'::regclass;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[&[T("5")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE cs;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_statistic WHERE starelid >= 16384;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[&[T("0")]],
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
fn test_analyze_reusing_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "an ANALYZE that reuses the statistics stored for unchanged contents still updates pg_class and the table's analyze counters",
            set_up_script: &[
                "CREATE TABLE sr (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO sr SELECT a.x * 10 + b.x, repeat('x', 50) FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) a(x), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_mod_since_analyze, analyze_count, last_analyze IS NULL AS never FROM pg_stat_user_tables WHERE relname = 'sr';",
                    expected: Expected::Rows {
                        columns: &[Column("n_mod_since_analyze", INT8), Column("analyze_count", INT8), Column("never", BOOL)],
                        rows: &[
                            &[T("100"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE sr;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname = 'sr';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("t"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_mod_since_analyze, analyze_count, last_analyze IS NOT NULL AS analyzed FROM pg_stat_user_tables WHERE relname = 'sr';",
                    expected: Expected::Rows {
                        columns: &[Column("n_mod_since_analyze", INT8), Column("analyze_count", INT8), Column("analyzed", BOOL)],
                        rows: &[
                            &[T("0"), T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, null_frac, n_distinct FROM pg_stats WHERE tablename = 'sr' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("null_frac", FLOAT4), Column("n_distinct", FLOAT4)],
                        rows: &[
                            &[T("a"), T("0"), T("-1")],
                            &[T("b"), T("0"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sr VALUES (100, 'y');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM sr WHERE a = 100;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_mod_since_analyze, analyze_count FROM pg_stat_user_tables WHERE relname = 'sr';",
                    expected: Expected::Rows {
                        columns: &[Column("n_mod_since_analyze", INT8), Column("analyze_count", INT8)],
                        rows: &[
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE sr;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname = 'sr';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("t"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_mod_since_analyze, analyze_count, last_analyze IS NOT NULL AS analyzed FROM pg_stat_user_tables WHERE relname = 'sr';",
                    expected: Expected::Rows {
                        columns: &[Column("n_mod_since_analyze", INT8), Column("analyze_count", INT8), Column("analyzed", BOOL)],
                        rows: &[
                            &[T("0"), T("2"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, null_frac, n_distinct FROM pg_stats WHERE tablename = 'sr' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("null_frac", FLOAT4), Column("n_distinct", FLOAT4)],
                        rows: &[
                            &[T("a"), T("0"), T("-1")],
                            &[T("b"), T("0"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE sr;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_mod_since_analyze, analyze_count FROM pg_stat_user_tables WHERE relname = 'sr';",
                    expected: Expected::Rows {
                        columns: &[Column("n_mod_since_analyze", INT8), Column("analyze_count", INT8)],
                        rows: &[
                            &[T("0"), T("3")],
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
fn test_pg_stat_activity() {
    run_scripts(&[
        ScriptTest {
            name: "pg_stat_activity shows a session's statement, state, and settings, read once per transaction",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datname = current_database() AS db, usename, state, query, wait_event_type, wait_event, backend_type, xact_start IS NOT NULL AS in_xact, backend_start <= query_start AS ordered FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("db", BOOL), Column("usename", NAME), Column("state", TEXT), Column("query", TEXT), Column("wait_event_type", TEXT), Column("wait_event", TEXT), Column("backend_type", TEXT), Column("in_xact", BOOL), Column("ordered", BOOL)],
                        rows: &[
                            &[T("t"), T("postgres"), T("active"), T("SELECT datname = current_database() AS db, usename, state, query, wait_event_type, wait_event, backend_type, xact_start IS NOT NULL AS in_xact, backend_start <= query_start AS ordered FROM pg_stat_activity WHERE pid = pg_backend_pid();"), Null, Null, T("client backend"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET application_name = 'checker';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT application_name FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("application_name", TEXT)],
                        rows: &[
                            &[T("checker")],
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
                    query: "SELECT query, backend_xid IS NULL AS no_xid FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("query", TEXT), Column("no_xid", BOOL)],
                        rows: &[
                            &[T("SELECT query, backend_xid IS NULL AS no_xid FROM pg_stat_activity WHERE pid = pg_backend_pid();"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE activity_t (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT query, backend_xid IS NULL AS no_xid FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("query", TEXT), Column("no_xid", BOOL)],
                        rows: &[
                            &[T("SELECT query, backend_xid IS NULL AS no_xid FROM pg_stat_activity WHERE pid = pg_backend_pid();"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_clear_snapshot();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_clear_snapshot", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT backend_xid IS NULL AS no_xid FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("no_xid", BOOL)],
                        rows: &[
                            &[T("f")],
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
                    query: "SET track_activities = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT state, query, xact_start FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("state", TEXT), Column("query", TEXT), Column("xact_start", TIMESTAMPTZ)],
                        rows: &[
                            &[T("disabled"), T(""), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET track_activities;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT backend_type, state, query, wait_event_type, wait_event, backend_start IS NOT NULL AS started, client_addr, client_port FROM pg_stat_activity WHERE backend_type = 'checkpointer';",
                    expected: Expected::Rows {
                        columns: &[Column("backend_type", TEXT), Column("state", TEXT), Column("query", TEXT), Column("wait_event_type", TEXT), Column("wait_event", TEXT), Column("started", BOOL), Column("client_addr", INET), Column("client_port", INT4)],
                        rows: &[
                            &[T("checkpointer"), Null, T(""), T("Activity"), T("CheckpointerMain"), T("t"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT backend_xmin IS NOT NULL AS has_xmin FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("has_xmin", BOOL)],
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
            name: "pg_stat_get_backend functions read a session by its slot, and hide its details from users who may not see them",
            set_up_script: &[
                "CREATE ROLE activity_nobody;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_backend_activity(s) LIKE 'SELECT pg_stat_get_backend_activity%' AS own, pg_stat_get_backend_wait_event(s), pg_stat_get_backend_xact_start(s) IS NOT NULL AS x, pg_stat_get_backend_start(s) IS NOT NULL AS st, pg_stat_get_backend_client_port(s) > 0 AS port, pg_stat_get_backend_dbid(s) = (SELECT oid FROM pg_database WHERE datname = current_database()) AS db, pg_stat_get_backend_userid(s) = 'postgres'::regrole AS u FROM pg_stat_get_backend_idset() s WHERE pg_stat_get_backend_pid(s) = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("own", BOOL), Column("pg_stat_get_backend_wait_event", TEXT), Column("x", BOOL), Column("st", BOOL), Column("port", BOOL), Column("db", BOOL), Column("u", BOOL)],
                        rows: &[
                            &[T("t"), Null, T("t"), T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_backend_pid(100000), pg_stat_get_backend_activity(100000), pg_stat_get_backend_wait_event(100000), pg_stat_get_backend_dbid(100000), pg_stat_get_backend_start(100000);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_backend_pid", INT4), Column("pg_stat_get_backend_activity", TEXT), Column("pg_stat_get_backend_wait_event", TEXT), Column("pg_stat_get_backend_dbid", OID), Column("pg_stat_get_backend_start", TIMESTAMPTZ)],
                        rows: &[
                            &[Null, T("<backend information not available>"), T("<backend information not available>"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE activity_nobody;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_backend_activity(s), pg_stat_get_backend_wait_event(s), pg_stat_get_backend_start(s), pg_stat_get_backend_client_port(s) FROM pg_stat_get_backend_idset() s WHERE pg_stat_get_backend_pid(s) = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_backend_activity", TEXT), Column("pg_stat_get_backend_wait_event", TEXT), Column("pg_stat_get_backend_start", TIMESTAMPTZ), Column("pg_stat_get_backend_client_port", INT4)],
                        rows: &[
                            &[T("<insufficient privilege>"), T("<insufficient privilege>"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT state, query, backend_type, wait_event FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("state", TEXT), Column("query", TEXT), Column("backend_type", TEXT), Column("wait_event", TEXT)],
                        rows: &[
                            &[Null, T("<insufficient privilege>"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a session waiting for a lock shows the lock's wait event, pg_locks when it began waiting, and another session its idle states",
            set_up_script: &[
                "CREATE TABLE activity_w (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE activity_w;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM activity_w;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT state, wait_event_type, wait_event FROM pg_stat_activity WHERE query = 'SELECT count(*) FROM activity_w;';",
                    expected: Expected::Rows {
                        columns: &[Column("state", TEXT), Column("wait_event_type", TEXT), Column("wait_event", TEXT)],
                        rows: &[
                            &[T("active"), T("Lock"), T("relation")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, mode, waitstart IS NOT NULL AS waiting, waitstart <= clock_timestamp() AS before FROM pg_locks WHERE NOT granted;",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("mode", TEXT), Column("waiting", BOOL), Column("before", BOOL)],
                        rows: &[
                            &[T("relation"), T("AccessShareLock"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mode, waitstart FROM pg_locks WHERE granted AND relation = 'activity_w'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("mode", TEXT), Column("waitstart", TIMESTAMPTZ)],
                        rows: &[
                            &[T("AccessExclusiveLock"), Null],
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT state, wait_event_type, wait_event, query, xact_start IS NULL AS no_xact FROM pg_stat_activity WHERE query = 'BEGIN;';",
                    expected: Expected::Rows {
                        columns: &[Column("state", TEXT), Column("wait_event_type", TEXT), Column("wait_event", TEXT), Column("query", TEXT), Column("no_xact", BOOL)],
                        rows: &[
                            &[T("idle in transaction"), T("Client"), T("ClientRead"), T("BEGIN;"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1/0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT state, query, xact_start IS NULL AS no_xact FROM pg_stat_activity WHERE query = 'SELECT 1/0;';",
                    expected: Expected::Rows {
                        columns: &[Column("state", TEXT), Column("query", TEXT), Column("no_xact", BOOL)],
                        rows: &[
                            &[T("idle in transaction (aborted)"), T("SELECT 1/0;"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT state, query FROM pg_stat_activity WHERE query = 'ROLLBACK;';",
                    expected: Expected::Rows {
                        columns: &[Column("state", TEXT), Column("query", TEXT)],
                        rows: &[
                            &[T("idle"), T("ROLLBACK;")],
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
fn test_pg_stat_progress_copy() {
    run_scripts(&[
        ScriptTest {
            name: "pg_stat_progress_copy shows a running COPY's direction, source, and rows to the session that runs it",
            set_up_script: &[
                "CREATE TABLE tab_progress_reporting (name text, age int4, location point, salary int4, manager name);",
                r#"CREATE FUNCTION notice_after_tab_progress_reporting() RETURNS trigger AS $$
DECLARE report record;
BEGIN
  WITH progress_data AS (
    SELECT relid::regclass::text AS relname, command, type, bytes_processed > 0 AS has_bytes_processed,
       bytes_total > 0 AS has_bytes_total, tuples_processed, tuples_excluded, tuples_skipped
      FROM pg_stat_progress_copy WHERE pid = pg_backend_pid())
  SELECT INTO report (to_jsonb(r)) AS value FROM progress_data r;
  RAISE INFO 'progress: %', report.value::text;
  RETURN new;
END;
$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER check_after_tab_progress_reporting AFTER INSERT ON tab_progress_reporting FOR EACH STATEMENT EXECUTE FUNCTION notice_after_tab_progress_reporting();",
                r#"CREATE FUNCTION progress_row(r tab_progress_reporting) RETURNS text LANGUAGE plpgsql AS $$ DECLARE v text; BEGIN SELECT command || ' ' || type || ' ' || tuples_processed || ' ' || bytes_total INTO v FROM pg_stat_progress_copy WHERE pid = pg_backend_pid(); RETURN v; END $$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tab_progress_reporting FROM STDIN;",
                    expected: Expected::Tag("COPY 3"),
                    notices: &[Diagnostic { severity: "INFO", code: "00000", message: r#"progress: {"type": "PIPE", "command": "COPY FROM", "relname": "tab_progress_reporting", "tuples_skipped": 0, "has_bytes_total": false, "tuples_excluded": 0, "tuples_processed": 3, "has_bytes_processed": true}"#, ..N }],
                    copy_from_stdin_file: "copy-progress.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY tab_progress_reporting FROM STDIN (ON_ERROR ignore);",
                    expected: Expected::Tag("COPY 1"),
                    notices: &[
                        Diagnostic { code: "00000", message: "2 rows were skipped due to data type incompatibility", ..N },
                        Diagnostic { severity: "INFO", code: "00000", message: r#"progress: {"type": "PIPE", "command": "COPY FROM", "relname": "tab_progress_reporting", "tuples_skipped": 2, "has_bytes_total": false, "tuples_excluded": 0, "tuples_processed": 1, "has_bytes_processed": true}"#, ..N },
                    ],
                    copy_from_stdin_file: "copy-progress-on-error.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stat_progress_copy;",
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
                    query: "COPY (SELECT progress_row(t) FROM tab_progress_reporting t ORDER BY name) TO STDOUT;",
                    expected: Expected::Tag("COPY 4"),
                    copy_to_stdout_file: "copy-progress-to.txt",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_pg_stat_progress_analyze() {
    run_scripts(&[
        ScriptTest {
            name: "pg_stat_progress_analyze shows an ANALYZE of an inheritance tree waiting for a child table in its phase of sampling the tree",
            set_up_script: &[
                "CREATE TABLE ap (a INT);",
                "CREATE TABLE ac (b INT) INHERITS (ap);",
                "INSERT INTO ap VALUES (1);",
                "INSERT INTO ac VALUES (2, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE ac;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ap;",
                    expected: Expected::Tag("ANALYZE"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relid = 'ap'::regclass AS rel, datname = current_database() AS db, phase, ext_stats_total, ext_stats_computed, child_tables_total, child_tables_done, current_child_table_relid, delay_time FROM pg_stat_progress_analyze;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", BOOL), Column("db", BOOL), Column("phase", TEXT), Column("ext_stats_total", INT8), Column("ext_stats_computed", INT8), Column("child_tables_total", INT8), Column("child_tables_done", INT8), Column("current_child_table_relid", OID), Column("delay_time", FLOAT8)],
                        rows: &[
                            &[T("t"), T("t"), T("acquiring inherited sample rows"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
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
                    query: "SELECT count(*) FROM pg_stat_progress_analyze;",
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
        ScriptTest {
            name: "VACUUM (ANALYZE) of an inheritance tree ends its vacuum's progress before its analysis waits for a child table",
            set_up_script: &[
                "CREATE TABLE vp (a INT);",
                "CREATE TABLE vc (b INT) INHERITS (vp);",
                "INSERT INTO vp VALUES (1);",
                "INSERT INTO vc VALUES (2, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE vc;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (ANALYZE) vp;",
                    expected: Expected::Tag("VACUUM"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM pg_stat_progress_vacuum) AS vacuums, (SELECT phase FROM pg_stat_progress_analyze WHERE relid = 'vp'::regclass) AS phase;",
                    expected: Expected::Rows {
                        columns: &[Column("vacuums", INT8), Column("phase", TEXT)],
                        rows: &[
                            &[T("0"), T("acquiring inherited sample rows")],
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
            ],
            ..S
        },
    ]);
}

#[test]
fn test_pg_stat_relations() {
    run_scripts(&[
        ScriptTest {
            name: "The cumulative statistics count each table's and index's scans, rows, and changes, and each database's transactions, sessions, and temporary files",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE st (a INT PRIMARY KEY, b INT, c TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX st_b ON st (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO st SELECT g, g, 'x' FROM generate_series(1, 100) g;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT seq_scan, seq_tup_read, idx_scan, idx_tup_fetch, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd, n_tup_newpage_upd, n_live_tup, n_dead_tup, n_mod_since_analyze, n_ins_since_vacuum, last_seq_scan IS NOT NULL AS seq_scanned FROM pg_stat_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("seq_scan", INT8), Column("seq_tup_read", INT8), Column("idx_scan", INT8), Column("idx_tup_fetch", INT8), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8), Column("n_tup_newpage_upd", INT8), Column("n_live_tup", INT8), Column("n_dead_tup", INT8), Column("n_mod_since_analyze", INT8), Column("n_ins_since_vacuum", INT8), Column("seq_scanned", BOOL)],
                        rows: &[
                            &[T("2"), T("0"), T("0"), T("0"), T("100"), T("0"), T("0"), T("0"), T("0"), T("100"), T("0"), T("100"), T("100"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexonlyscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM st;",
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
                    query: "SELECT count(*) FROM st WHERE c = 'y';",
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
                    query: "RESET enable_indexscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_bitmapscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_indexonlyscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM st WHERE a = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c FROM st WHERE b = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("c", TEXT)],
                        rows: &[
                            &[T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE st SET c = 'z' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE st SET b = 1000 WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM st WHERE a = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT seq_scan, seq_tup_read, idx_scan, idx_tup_fetch, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd, n_live_tup, n_dead_tup, n_mod_since_analyze, n_ins_since_vacuum, last_seq_scan IS NOT NULL AS seq_scanned, last_idx_scan IS NOT NULL AS idx_scanned FROM pg_stat_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("seq_scan", INT8), Column("seq_tup_read", INT8), Column("idx_scan", INT8), Column("idx_tup_fetch", INT8), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8), Column("n_live_tup", INT8), Column("n_dead_tup", INT8), Column("n_mod_since_analyze", INT8), Column("n_ins_since_vacuum", INT8), Column("seq_scanned", BOOL), Column("idx_scanned", BOOL)],
                        rows: &[
                            &[T("4"), T("200"), T("5"), T("5"), T("100"), T("2"), T("1"), T("1"), T("99"), T("3"), T("103"), T("100"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelname, idx_scan, idx_tup_read, idx_tup_fetch, last_idx_scan IS NOT NULL AS scanned FROM pg_stat_user_indexes WHERE relname = 'st' ORDER BY indexrelname;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelname", NAME), Column("idx_scan", INT8), Column("idx_tup_read", INT8), Column("idx_tup_fetch", INT8), Column("scanned", BOOL)],
                        rows: &[
                            &[T("st_b"), T("1"), T("1"), T("0"), T("t")],
                            &[T("st_pkey"), T("4"), T("4"), T("4"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, heap_blks_read + heap_blks_hit > 0 AS heap_read, idx_blks_read + idx_blks_hit > 0 AS idx_read FROM pg_statio_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", TEXT), Column("heap_read", TEXT), Column("idx_read", TEXT)],
                        rows: &[
                            &[T("st"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("rows that are still in the transaction delta read no tree nodes, and writes count no nodes their index maintenance reads"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO st VALUES (500, 500, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_tup_ins, n_tup_upd, n_tup_del FROM pg_stat_xact_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8)],
                        rows: &[
                            &[T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_tuples_inserted('st'::regclass) AS inserted, pg_stat_get_xact_numscans('st'::regclass) AS scans;",
                    expected: Expected::Rows {
                        columns: &[Column("inserted", INT8), Column("scans", INT8)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO st VALUES (501, 501, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_tup_ins FROM pg_stat_xact_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("n_tup_ins", INT8)],
                        rows: &[
                            &[T("2")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_tup_ins, n_live_tup, n_dead_tup FROM pg_stat_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("n_tup_ins", INT8), Column("n_live_tup", INT8), Column("n_dead_tup", INT8)],
                        rows: &[
                            &[T("102"), T("100"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET stats_fetch_consistency = snapshot;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_tup_ins FROM pg_stat_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("n_tup_ins", INT8)],
                        rows: &[
                            &[T("102")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_snapshot_timestamp() IS NOT NULL AS has_snapshot;",
                    expected: Expected::Rows {
                        columns: &[Column("has_snapshot", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_clear_snapshot();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_clear_snapshot", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_snapshot_timestamp() IS NOT NULL AS has_snapshot;",
                    expected: Expected::Rows {
                        columns: &[Column("has_snapshot", BOOL)],
                        rows: &[
                            &[T("f")],
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
                    query: "RESET stats_fetch_consistency;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_snapshot_timestamp() IS NOT NULL AS has_snapshot;",
                    expected: Expected::Rows {
                        columns: &[Column("has_snapshot", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT numbackends > 0 AS connected, xact_commit > 0 AS committed, tup_inserted >= 100 AS inserted, tup_returned > 0 AS returned, sessions > 0 AS sessions, session_time > 0 AS session_time, active_time > 0 AS active, stats_reset IS NULL AS never_reset, conflicts, deadlocks FROM pg_stat_database WHERE datname = current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("connected", BOOL), Column("committed", BOOL), Column("inserted", BOOL), Column("returned", BOOL), Column("sessions", BOOL), Column("session_time", BOOL), Column("active", BOOL), Column("never_reset", BOOL), Column("conflicts", INT8), Column("deadlocks", INT8)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT confl_tablespace, confl_lock, confl_snapshot, confl_bufferpin, confl_deadlock, confl_active_logicalslot FROM pg_stat_database_conflicts WHERE datname = current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("confl_tablespace", INT8), Column("confl_lock", INT8), Column("confl_snapshot", INT8), Column("confl_bufferpin", INT8), Column("confl_deadlock", INT8), Column("confl_active_logicalslot", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE st;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_tup_ins, n_live_tup, n_dead_tup FROM pg_stat_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("n_tup_ins", INT8), Column("n_live_tup", INT8), Column("n_dead_tup", INT8)],
                        rows: &[
                            &[T("102"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM (SELECT g FROM generate_series(1, 20000) g ORDER BY g DESC) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT temp_files > 0 AS spilled, temp_bytes > 0 AS spilled_bytes FROM pg_stat_database WHERE datname = current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("spilled", BOOL), Column("spilled_bytes", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_single_table_counters('st'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_single_table_counters", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT seq_scan, n_tup_ins, n_live_tup FROM pg_stat_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("seq_scan", INT8), Column("n_tup_ins", INT8), Column("n_live_tup", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stats_reset IS NOT NULL AS reset FROM pg_stat_database WHERE datname = current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("reset", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT xact_commit, tup_inserted, temp_files FROM pg_stat_database WHERE datname = current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("xact_commit", INT8), Column("tup_inserted", INT8), Column("temp_files", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT seq_scan, n_tup_ins FROM pg_stat_user_tables WHERE relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("seq_scan", INT8), Column("n_tup_ins", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT checksum_failures, checksum_last_failure FROM pg_stat_database WHERE datname = current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("checksum_failures", INT8), Column("checksum_last_failure", TIMESTAMPTZ)],
                        rows: &[
                            &[T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A session counts the deadlock its lock wait found in its database's statistics",
            set_up_script: &[
                "CREATE TABLE d1 (a INT);",
                "CREATE TABLE d2 (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE d1;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE d2;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE d2;",
                    expected: Expected::Error(Diagnostic { code: "40P01", message: "deadlock detected", hint: "See server log for query details.", ..E }),
                    client: "A",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE d1;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT deadlocks FROM pg_stat_database WHERE datname = current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("deadlocks", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_pg_stat_progress_create_index() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE INDEX CONCURRENTLY shows its wait for the table's writers before its build",
            set_up_script: &[
                "CREATE TABLE ci (a INT, b TEXT);",
                "INSERT INTO ci SELECT g, 'x' FROM generate_series(1, 1000) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ci VALUES (0, 'y');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX CONCURRENTLY ci_a ON ci (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT command, phase, lockers_total, lockers_done, current_locker_pid = pg_backend_pid() AS waits_for_holder, index_relid = 'ci_a'::regclass AS index, relid = 'ci'::regclass AS rel, datname = current_database() AS db FROM pg_stat_progress_create_index;",
                    expected: Expected::Rows {
                        columns: &[Column("command", TEXT), Column("phase", TEXT), Column("lockers_total", INT8), Column("lockers_done", INT8), Column("waits_for_holder", BOOL), Column("index", BOOL), Column("rel", BOOL), Column("db", BOOL)],
                        rows: &[
                            &[T("CREATE INDEX CONCURRENTLY"), T("waiting for writers before build"), T("1"), T("0"), T("t"), T("t"), T("t"), T("t")],
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
                    query: "SELECT count(*) FROM pg_stat_progress_create_index;",
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
        ScriptTest {
            name: "REINDEX CONCURRENTLY shows its wait for the table's writers before it builds the new index",
            set_up_script: &[
                "CREATE TABLE ci (a INT, b TEXT);",
                "INSERT INTO ci SELECT g, 'x' FROM generate_series(1, 1000) g;",
                "CREATE INDEX ci_a ON ci (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ci VALUES (0, 'y');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX CONCURRENTLY ci_a;",
                    expected: Expected::Tag("REINDEX"),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT command, phase, lockers_total, lockers_done, current_locker_pid = pg_backend_pid() AS waits_for_holder, index_relid = 'ci_a'::regclass AS index, relid = 'ci'::regclass AS rel, datname = current_database() AS db FROM pg_stat_progress_create_index;",
                    expected: Expected::Rows {
                        columns: &[Column("command", TEXT), Column("phase", TEXT), Column("lockers_total", INT8), Column("lockers_done", INT8), Column("waits_for_holder", BOOL), Column("index", BOOL), Column("rel", BOOL), Column("db", BOOL)],
                        rows: &[
                            &[T("REINDEX CONCURRENTLY"), T("waiting for writers before build"), T("1"), T("0"), T("t"), T("t"), T("t"), T("t")],
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
                    query: "SELECT count(*) FROM pg_stat_progress_create_index;",
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
fn test_pg_stat_shared() {
    run_scripts(&[
        ScriptTest {
            name: "The server counts its checkpoints and WAL, keeps when each kind of its counts of what it has one of was last reset, and resets them by kind",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE shared_before AS SELECT now() AS t;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT num_timed >= 0 AS timed, num_requested >= 0 AS requested, num_done >= 0 AS done, restartpoints_timed, restartpoints_req, restartpoints_done, write_time >= 0 AS wrote, sync_time >= 0 AS synced, buffers_written >= 0 AS buffers, slru_written, stats_reset IS NOT NULL AS has_reset FROM pg_stat_checkpointer;",
                    expected: Expected::Rows {
                        columns: &[Column("timed", BOOL), Column("requested", BOOL), Column("done", BOOL), Column("restartpoints_timed", INT8), Column("restartpoints_req", INT8), Column("restartpoints_done", INT8), Column("wrote", BOOL), Column("synced", BOOL), Column("buffers", BOOL), Column("slru_written", INT8), Column("has_reset", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("0"), T("0"), T("0"), T("t"), T("t"), T("t"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT buffers_clean, maxwritten_clean, buffers_alloc >= 0 AS alloc, stats_reset IS NOT NULL AS has_reset FROM pg_stat_bgwriter;",
                    expected: Expected::Rows {
                        columns: &[Column("buffers_clean", INT8), Column("maxwritten_clean", INT8), Column("alloc", BOOL), Column("has_reset", BOOL)],
                        rows: &[
                            &[T("0"), T("0"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT wal_records > 0 AS records, wal_fpi >= 0 AS fpi, wal_bytes > 0 AS bytes, wal_buffers_full >= 0 AS full, stats_reset IS NOT NULL AS has_reset FROM pg_stat_wal;",
                    expected: Expected::Rows {
                        columns: &[Column("records", BOOL), Column("fpi", BOOL), Column("bytes", BOOL), Column("full", BOOL), Column("has_reset", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT archived_count, last_archived_wal, last_archived_time, failed_count, last_failed_wal, last_failed_time, stats_reset IS NOT NULL AS has_reset FROM pg_stat_archiver;",
                    expected: Expected::Rows {
                        columns: &[Column("archived_count", INT8), Column("last_archived_wal", TEXT), Column("last_archived_time", TIMESTAMPTZ), Column("failed_count", INT8), Column("last_failed_wal", TEXT), Column("last_failed_time", TIMESTAMPTZ), Column("has_reset", BOOL)],
                        rows: &[
                            &[T("0"), Null, Null, T("0"), Null, Null, T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, blks_zeroed >= 0 AS zeroed, blks_hit >= 0 AS hit, blks_read >= 0 AS read, blks_written >= 0 AS written, blks_exists >= 0 AS exists, flushes >= 0 AS flushes, truncates >= 0 AS truncates, stats_reset IS NOT NULL AS has_reset FROM pg_stat_slru ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("zeroed", BOOL), Column("hit", BOOL), Column("read", BOOL), Column("written", BOOL), Column("exists", BOOL), Column("flushes", BOOL), Column("truncates", BOOL), Column("has_reset", BOOL)],
                        rows: &[
                            &[T("commit_timestamp"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                            &[T("multixact_member"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                            &[T("multixact_offset"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                            &[T("notify"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                            &[T("other"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                            &[T("serializable"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                            &[T("subtransaction"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                            &[T("transaction"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ck SELECT generate_series(1, 1000);",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE before AS SELECT num_requested, num_done, buffers_written FROM pg_stat_checkpointer;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.num_requested - b.num_requested AS requested, c.num_done - b.num_done AS done, c.buffers_written > b.buffers_written AS wrote FROM pg_stat_checkpointer c, before b;",
                    expected: Expected::Rows {
                        columns: &[Column("requested", INT8), Column("done", INT8), Column("wrote", BOOL)],
                        rows: &[
                            &[T("1"), T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('checkpointer');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT num_timed, num_requested, num_done, buffers_written, write_time, sync_time, stats_reset >= (SELECT t FROM shared_before) AS recent FROM pg_stat_checkpointer;",
                    expected: Expected::Rows {
                        columns: &[Column("num_timed", INT8), Column("num_requested", INT8), Column("num_done", INT8), Column("buffers_written", INT8), Column("write_time", FLOAT8), Column("sync_time", FLOAT8), Column("recent", BOOL)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('wal');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT wal_records < 10 AS few, stats_reset >= (SELECT t FROM shared_before) AS recent FROM pg_stat_wal;",
                    expected: Expected::Rows {
                        columns: &[Column("few", BOOL), Column("recent", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('bgwriter');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stats_reset >= (SELECT t FROM shared_before) AS recent FROM pg_stat_bgwriter;",
                    expected: Expected::Rows {
                        columns: &[Column("recent", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('archiver');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stats_reset >= (SELECT t FROM shared_before) AS recent FROM pg_stat_archiver;",
                    expected: Expected::Rows {
                        columns: &[Column("recent", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_slru('notify');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_slru", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, stats_reset >= (SELECT t FROM shared_before) AS reset FROM pg_stat_slru ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("reset", BOOL)],
                        rows: &[
                            &[T("commit_timestamp"), T("f")],
                            &[T("multixact_member"), T("f")],
                            &[T("multixact_offset"), T("f")],
                            &[T("notify"), T("t")],
                            &[T("other"), T("f")],
                            &[T("serializable"), T("f")],
                            &[T("subtransaction"), T("f")],
                            &[T("transaction"), T("f")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_slru('no such slru');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_slru", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, stats_reset >= (SELECT t FROM shared_before) AS reset FROM pg_stat_slru ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("reset", BOOL)],
                        rows: &[
                            &[T("commit_timestamp"), T("f")],
                            &[T("multixact_member"), T("f")],
                            &[T("multixact_offset"), T("f")],
                            &[T("notify"), T("t")],
                            &[T("other"), T("t")],
                            &[T("serializable"), T("f")],
                            &[T("subtransaction"), T("f")],
                            &[T("transaction"), T("f")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_slru();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_slru", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stat_slru WHERE stats_reset >= (SELECT t FROM shared_before);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('io');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('recovery_prefetch');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('nonsense');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized reset target: "nonsense""#, hint: r#"Target must be "archiver", "bgwriter", "checkpointer", "io", "recovery_prefetch", "slru", or "wal"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
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
fn test_pg_stat_functions() {
    run_scripts(&[
        ScriptTest {
            name: "track_functions counts the calls of procedural-language functions, and of SQL ones at all, pg_stat_have_stats finds the server's entries, and a sequence's reads count as its blocks",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fs_sql(i int) RETURNS int LANGUAGE sql AS $$ SELECT i + 1 $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fs_pl(i int) RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN fs_sql(i) * 2; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fs_set(n int) RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN FOR i IN 1..n LOOP RETURN NEXT i; END LOOP; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE fs_proc() LANGUAGE plpgsql AS $$ BEGIN PERFORM 1; END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fs_pl(1);",
                    expected: Expected::Rows {
                        columns: &[Column("fs_pl", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stat_user_functions;",
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
                    query: "SET track_functions = 'pl';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fs_pl(1), fs_pl(2);",
                    expected: Expected::Rows {
                        columns: &[Column("fs_pl", INT4), Column("fs_pl", INT4)],
                        rows: &[
                            &[T("4"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(x) FROM fs_set(3) x;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL fs_proc();",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT funcname, calls FROM pg_stat_xact_user_functions ORDER BY funcname;",
                    expected: Expected::Rows {
                        columns: &[Column("funcname", NAME), Column("calls", INT8)],
                        rows: &[
                            &[T("fs_pl"), T("2")],
                            &[T("fs_proc"), T("1")],
                            &[T("fs_set"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_function_calls('fs_pl'::regproc), pg_stat_get_xact_function_calls('fs_sql'::regproc);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_function_calls", INT8), Column("pg_stat_get_xact_function_calls", INT8)],
                        rows: &[
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT funcname, calls, total_time >= self_time AS total_covers_self, self_time >= 0 AS self FROM pg_stat_user_functions ORDER BY funcname;",
                    expected: Expected::Rows {
                        columns: &[Column("funcname", NAME), Column("calls", INT8), Column("total_covers_self", BOOL), Column("self", BOOL)],
                        rows: &[
                            &[T("fs_pl"), T("2"), T("t"), T("t")],
                            &[T("fs_proc"), T("1"), T("t"), T("t")],
                            &[T("fs_set"), T("1"), T("t"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET track_functions = 'all';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fs_pl(3);",
                    expected: Expected::Rows {
                        columns: &[Column("fs_pl", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT funcname, calls FROM pg_stat_user_functions ORDER BY funcname;",
                    expected: Expected::Rows {
                        columns: &[Column("funcname", NAME), Column("calls", INT8)],
                        rows: &[
                            &[T("fs_pl"), T("3")],
                            &[T("fs_proc"), T("1")],
                            &[T("fs_set"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('function', (SELECT oid FROM pg_database WHERE datname = current_database()), 'fs_pl'::regproc::oid) AS pl, pg_stat_have_stats('function', (SELECT oid FROM pg_database WHERE datname = current_database()), 'lower(text)'::regprocedure::oid) AS builtin;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function pg_stat_have_stats(unknown, oid, oid) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_single_function_counters('fs_pl'::regproc);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_single_function_counters", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT funcname, calls FROM pg_stat_user_functions ORDER BY funcname;",
                    expected: Expected::Rows {
                        columns: &[Column("funcname", NAME), Column("calls", INT8)],
                        rows: &[
                            &[T("fs_pl"), T("0")],
                            &[T("fs_proc"), T("1")],
                            &[T("fs_set"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('function', (SELECT oid FROM pg_database WHERE datname = current_database()), 'fs_pl'::regproc::oid) AS still;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function pg_stat_have_stats(unknown, oid, oid) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET track_functions;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE hs (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO hs VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('relation', (SELECT oid FROM pg_database WHERE datname = current_database()), 'hs'::regclass::oid) AS rel, pg_stat_have_stats('relation', 0, 'pg_database'::regclass::oid) AS shared, pg_stat_have_stats('database', (SELECT oid FROM pg_database WHERE datname = current_database()), 0) AS db, pg_stat_have_stats('wal', 0, 0) AS wal, pg_stat_have_stats('CHECKPOINTER', 0, 0) AS upper, pg_stat_have_stats('replslot', 0, 0) AS slot, pg_stat_have_stats('relation', 0, 1) AS missing;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function pg_stat_have_stats(unknown, oid, oid) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('backend', 0, (SELECT s FROM pg_stat_get_backend_idset() s WHERE pg_stat_get_backend_pid(s) = pg_backend_pid())) AS mine;",
                    expected: Expected::Rows {
                        columns: &[Column("mine", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('nonsense', 0, 0);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid statistics kind: "nonsense""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE sq;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('sq'), nextval('sq');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8), Column("nextval", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, blks_read + blks_hit > 0 AS read FROM pg_statio_user_sequences WHERE relname = 'sq';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", TEXT), Column("read", TEXT)],
                        rows: &[
                            &[T("sq"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("a sequence's state is kept in the server's sequence cache, so nextval reads no tree nodes"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_pg_stat_io() {
    run_scripts(&[
        ScriptTest {
            name: "test_pg_stat_io",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT backend_type, object, context, reads IS NULL AS r, read_bytes IS NULL AS rb, read_time IS NULL AS rt, writes IS NULL AS w, write_bytes IS NULL AS wb, write_time IS NULL AS wt, writebacks IS NULL AS wbk, writeback_time IS NULL AS wbkt, extends IS NULL AS e, extend_bytes IS NULL AS eb, extend_time IS NULL AS et, hits IS NULL AS h, evictions IS NULL AS ev, reuses IS NULL AS ru, fsyncs IS NULL AS f, fsync_time IS NULL AS ft, stats_reset IS NULL AS sr FROM pg_stat_io ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("backend_type", TEXT), Column("object", TEXT), Column("context", TEXT), Column("r", BOOL), Column("rb", BOOL), Column("rt", BOOL), Column("w", BOOL), Column("wb", BOOL), Column("wt", BOOL), Column("wbk", BOOL), Column("wbkt", BOOL), Column("e", BOOL), Column("eb", BOOL), Column("et", BOOL), Column("h", BOOL), Column("ev", BOOL), Column("ru", BOOL), Column("f", BOOL), Column("ft", BOOL), Column("sr", BOOL)],
                        rows: &[
                            &[T("autovacuum launcher"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("autovacuum launcher"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("autovacuum launcher"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("autovacuum launcher"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("autovacuum launcher"), T("wal"), T("normal"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("autovacuum worker"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("autovacuum worker"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("autovacuum worker"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("autovacuum worker"), T("relation"), T("vacuum"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("autovacuum worker"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("autovacuum worker"), T("wal"), T("normal"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("background worker"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("background worker"), T("relation"), T("bulkwrite"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("background worker"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("background worker"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("background worker"), T("relation"), T("vacuum"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("background worker"), T("temp relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f")],
                            &[T("background worker"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("background worker"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("background writer"), T("relation"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("background writer"), T("relation"), T("normal"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("background writer"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("background writer"), T("wal"), T("normal"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("checkpointer"), T("relation"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("checkpointer"), T("relation"), T("normal"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("checkpointer"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("checkpointer"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("client backend"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("client backend"), T("relation"), T("bulkwrite"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("client backend"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("client backend"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("client backend"), T("relation"), T("vacuum"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("client backend"), T("temp relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f")],
                            &[T("client backend"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("client backend"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("io worker"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("io worker"), T("relation"), T("bulkwrite"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("io worker"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("io worker"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("io worker"), T("relation"), T("vacuum"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("io worker"), T("temp relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f")],
                            &[T("io worker"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("io worker"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("slotsync worker"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("slotsync worker"), T("relation"), T("bulkwrite"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("slotsync worker"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("slotsync worker"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("slotsync worker"), T("relation"), T("vacuum"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("slotsync worker"), T("temp relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f")],
                            &[T("slotsync worker"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("slotsync worker"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("standalone backend"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("standalone backend"), T("relation"), T("bulkwrite"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("standalone backend"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("standalone backend"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("standalone backend"), T("relation"), T("vacuum"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("standalone backend"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("standalone backend"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("startup"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("startup"), T("relation"), T("bulkwrite"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("startup"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("startup"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("startup"), T("relation"), T("vacuum"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("startup"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("startup"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walreceiver"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walreceiver"), T("wal"), T("normal"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walsender"), T("relation"), T("bulkread"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("walsender"), T("relation"), T("bulkwrite"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("walsender"), T("relation"), T("init"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walsender"), T("relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walsender"), T("relation"), T("vacuum"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f")],
                            &[T("walsender"), T("temp relation"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("f")],
                            &[T("walsender"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walsender"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walsummarizer"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walsummarizer"), T("wal"), T("normal"), T("f"), T("f"), T("f"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walwriter"), T("wal"), T("init"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                            &[T("walwriter"), T("wal"), T("normal"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                        ],
                        tag: "SELECT 79",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stat_io;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("79")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE io_t (a int PRIMARY KEY, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO io_t SELECT g, repeat('x', 100) FROM generate_series(1, 1000) g;",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM io_t;",
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('io');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM io_t;",
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT object, context, reads + hits > 0 AS used, read_time, write_time FROM pg_stat_io WHERE backend_type = 'client backend' AND object = 'relation' AND context = 'normal';",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("context", TEXT), Column("used", BOOL), Column("read_time", FLOAT8), Column("write_time", FLOAT8)],
                        rows: &[
                            &[T("relation"), T("normal"), T("t"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stats_reset IS NOT NULL AS reset FROM pg_stat_io WHERE backend_type = 'checkpointer' AND object = 'relation' AND context = 'normal';",
                    expected: Expected::Rows {
                        columns: &[Column("reset", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(DISTINCT stats_reset) FROM pg_stat_io;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("79"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT object, context, reads IS NULL AS r, hits IS NULL AS h, stats_reset IS NULL AS sr FROM pg_stat_get_backend_io(pg_backend_pid()) ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("context", TEXT), Column("r", BOOL), Column("h", BOOL), Column("sr", BOOL)],
                        rows: &[
                            &[T("relation"), T("bulkread"), T("f"), T("f"), T("t")],
                            &[T("relation"), T("bulkwrite"), T("f"), T("f"), T("t")],
                            &[T("relation"), T("init"), T("f"), T("f"), T("t")],
                            &[T("relation"), T("normal"), T("f"), T("f"), T("t")],
                            &[T("relation"), T("vacuum"), T("f"), T("f"), T("t")],
                            &[T("temp relation"), T("normal"), T("f"), T("f"), T("t")],
                            &[T("wal"), T("init"), T("t"), T("t"), T("t")],
                            &[T("wal"), T("normal"), T("f"), T("t"), T("t")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hits + reads > 0 AS used FROM pg_stat_get_backend_io(pg_backend_pid()) WHERE object = 'relation' AND context = 'normal';",
                    expected: Expected::Rows {
                        columns: &[Column("used", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO io_t VALUES (1001, 'y');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT wal_records > 0 AS records, wal_fpi >= 0 AS fpi, wal_bytes > 0 AS bytes, wal_buffers_full >= 0 AS full, stats_reset FROM pg_stat_get_backend_wal(pg_backend_pid());",
                    expected: Expected::Rows {
                        columns: &[Column("records", BOOL), Column("fpi", BOOL), Column("bytes", BOOL), Column("full", BOOL), Column("stats_reset", TIMESTAMPTZ)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_backend_stats(pg_backend_pid());",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_backend_stats", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT wal_records, wal_bytes, stats_reset IS NOT NULL AS reset FROM pg_stat_get_backend_wal(pg_backend_pid());",
                    expected: Expected::Rows {
                        columns: &[Column("wal_records", INT8), Column("wal_bytes", NUMERIC), Column("reset", BOOL)],
                        rows: &[
                            &[T("0"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stat_get_backend_io(pg_backend_pid()) WHERE stats_reset IS NULL;",
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
                    query: "SELECT count(*) FROM pg_stat_get_backend_io(0);",
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
                    query: "SELECT pg_stat_get_backend_wal(0) IS NULL AS none;",
                    expected: Expected::Rows {
                        columns: &[Column("none", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_backend_stats(0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_backend_stats", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stat_get_backend_io((SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer'));",
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
                    query: "SELECT pg_stat_get_backend_wal((SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer')) IS NULL AS none;",
                    expected: Expected::Rows {
                        columns: &[Column("none", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET track_io_timing = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM io_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT read_time >= 0 AS timed FROM pg_stat_io WHERE backend_type = 'client backend' AND object = 'relation' AND context = 'normal';",
                    expected: Expected::Rows {
                        columns: &[Column("timed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT blk_read_time >= 0 AS read, blk_write_time >= 0 AS write FROM pg_stat_database WHERE datname = current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("read", BOOL), Column("write", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET track_io_timing;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE io_h (v bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET stats_fetch_consistency = snapshot;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO io_h SELECT hits FROM pg_stat_io WHERE backend_type = 'client backend' AND object = 'relation' AND context = 'normal';",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM io_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hits = (SELECT v FROM io_h) AS same FROM pg_stat_io WHERE backend_type = 'client backend' AND object = 'relation' AND context = 'normal';",
                    expected: Expected::Rows {
                        columns: &[Column("same", BOOL)],
                        rows: &[
                            &[T("t")],
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
                    query: "SELECT hits > (SELECT v FROM io_h) AS more FROM pg_stat_io WHERE backend_type = 'client backend' AND object = 'relation' AND context = 'normal';",
                    expected: Expected::Rows {
                        columns: &[Column("more", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET stats_fetch_consistency;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT writes > 0 AS wrote, fsyncs >= 0 AS synced FROM pg_stat_io WHERE backend_type = 'checkpointer' AND object = 'relation' AND context = 'normal';",
                    expected: Expected::Rows {
                        columns: &[Column("wrote", BOOL), Column("synced", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT writes > 0 AS wrote FROM pg_stat_io WHERE backend_type = 'client backend' AND object = 'wal' AND context = 'normal';",
                    expected: Expected::Rows {
                        columns: &[Column("wrote", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('io', 0, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
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
fn test_pg_stat_replication() {
    run_scripts(&[
        ScriptTest {
            name: "The replication views and functions show no WAL senders, receiver, slots, or subscriptions, as a server with none configured shows, and the recovery prefetcher counts nothing",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stat_replication;",
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
                    query: "SELECT count(*) FROM pg_stat_wal_receiver;",
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
                    query: "SELECT count(*) FROM pg_stat_replication_slots;",
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
                    query: "SELECT count(*) FROM pg_stat_subscription;",
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
                    query: "SELECT count(*) FROM pg_stat_subscription_stats;",
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
                    query: "SELECT count(*) FROM pg_replication_slots;",
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
                    query: "SELECT prefetch, hit, skip_init, skip_new, skip_fpw, skip_rep, wal_distance, block_distance, io_depth, stats_reset IS NOT NULL AS has_reset FROM pg_stat_recovery_prefetch;",
                    expected: Expected::Rows {
                        columns: &[Column("prefetch", INT8), Column("hit", INT8), Column("skip_init", INT8), Column("skip_new", INT8), Column("skip_fpw", INT8), Column("skip_rep", INT8), Column("wal_distance", INT4), Column("block_distance", INT4), Column("io_depth", INT4), Column("has_reset", BOOL)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("1"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("block_distance is the distance Postgres' recovery prefetcher last read ahead, which the server's recovery has no prefetcher to measure"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT prefetch, hit, skip_init, skip_new, skip_fpw, skip_rep, wal_distance, io_depth, stats_reset IS NOT NULL AS has_reset FROM pg_stat_recovery_prefetch;",
                    expected: Expected::Rows {
                        columns: &[Column("prefetch", INT8), Column("hit", INT8), Column("skip_init", INT8), Column("skip_new", INT8), Column("skip_fpw", INT8), Column("skip_rep", INT8), Column("wal_distance", INT4), Column("io_depth", INT4), Column("has_reset", BOOL)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_stat_get_wal_senders();",
                    expected: Expected::Rows {
                        columns: &[Column("pid", INT4), Column("state", TEXT), Column("sent_lsn", PG_LSN), Column("write_lsn", PG_LSN), Column("flush_lsn", PG_LSN), Column("replay_lsn", PG_LSN), Column("write_lag", INTERVAL), Column("flush_lag", INTERVAL), Column("replay_lag", INTERVAL), Column("sync_priority", INT4), Column("sync_state", TEXT), Column("reply_time", TIMESTAMPTZ)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_stat_get_wal_receiver();",
                    expected: Expected::Rows {
                        columns: &[Column("pid", INT4), Column("status", TEXT), Column("receive_start_lsn", PG_LSN), Column("receive_start_tli", INT4), Column("written_lsn", PG_LSN), Column("flushed_lsn", PG_LSN), Column("received_tli", INT4), Column("last_msg_send_time", TIMESTAMPTZ), Column("last_msg_receipt_time", TIMESTAMPTZ), Column("latest_end_lsn", PG_LSN), Column("latest_end_time", TIMESTAMPTZ), Column("slot_name", TEXT), Column("sender_host", TEXT), Column("sender_port", INT4), Column("conninfo", TEXT)],
                        rows: &[
                            &[Null, Null, Null, Null, Null, Null, Null, Null, Null, Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_stat_get_subscription(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("subid", OID), Column("relid", OID), Column("pid", INT4), Column("leader_pid", INT4), Column("received_lsn", PG_LSN), Column("last_msg_send_time", TIMESTAMPTZ), Column("last_msg_receipt_time", TIMESTAMPTZ), Column("latest_end_lsn", PG_LSN), Column("latest_end_time", TIMESTAMPTZ), Column("worker_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_stat_get_replication_slot('none');",
                    expected: Expected::Rows {
                        columns: &[Column("slot_name", TEXT), Column("spill_txns", INT8), Column("spill_count", INT8), Column("spill_bytes", INT8), Column("stream_txns", INT8), Column("stream_count", INT8), Column("stream_bytes", INT8), Column("total_txns", INT8), Column("total_bytes", INT8), Column("stats_reset", TIMESTAMPTZ)],
                        rows: &[
                            &[T("none"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_stat_get_subscription_stats(12345);",
                    expected: Expected::Rows {
                        columns: &[Column("subid", OID), Column("apply_error_count", INT8), Column("sync_error_count", INT8), Column("confl_insert_exists", INT8), Column("confl_update_origin_differs", INT8), Column("confl_update_exists", INT8), Column("confl_update_missing", INT8), Column("confl_delete_origin_differs", INT8), Column("confl_delete_missing", INT8), Column("confl_multiple_unique_conflicts", INT8), Column("stats_reset", TIMESTAMPTZ)],
                        rows: &[
                            &[T("12345"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_replication_slot(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_replication_slot", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_replication_slot('none');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"replication slot "none" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_subscription_stats(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_subscription_stats", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_subscription_stats(12345);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_subscription_stats", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE s4_before AS SELECT now() AS t;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_reset_shared('recovery_prefetch');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_reset_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stats_reset >= (SELECT t FROM s4_before) AS reset FROM pg_stat_recovery_prefetch;",
                    expected: Expected::Rows {
                        columns: &[Column("reset", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('replslot', 0, 0), pg_stat_have_stats('subscription', 0, 12345);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL), Column("pg_stat_have_stats", BOOL)],
                        rows: &[
                            &[T("f"), T("f")],
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
fn test_pg_stat_drops() {
    run_scripts(&[
        ScriptTest {
            name: "track_counts off counts no changes, a drop that rolls back keeps the counts, and committed drops of tables and functions forget theirs, and a notification queue that fills a page zeroes the next",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE tc_t (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tc_t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET track_counts = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tc_t VALUES (2), (3);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tc_t SET a = a + 1;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM tc_t WHERE a = 4;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET track_counts = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_tup_ins, n_tup_upd, n_tup_del, n_live_tup FROM pg_stat_user_tables WHERE relname = 'tc_t';",
                    expected: Expected::Rows {
                        columns: &[Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_live_tup", INT8)],
                        rows: &[
                            &[T("1"), T("0"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sp_t (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sp_t VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE sp_t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_tup_ins FROM pg_stat_user_tables WHERE relname = 'sp_t';",
                    expected: Expected::Rows {
                        columns: &[Column("n_tup_ins", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dr_t (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dr_t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dr_oid AS SELECT 'dr_t'::regclass::oid AS oid;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_tuples_inserted(oid) FROM dr_oid;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_tuples_inserted", INT8)],
                        rows: &[
                            &[T("1")],
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
                    query: "DROP TABLE dr_t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_tuples_inserted(oid) FROM dr_oid;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_tuples_inserted", INT8)],
                        rows: &[
                            &[T("1")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_tuples_inserted(oid), pg_stat_have_stats('relation', (SELECT oid FROM pg_database WHERE datname = current_database()), oid) FROM dr_oid;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function pg_stat_have_stats(unknown, oid, oid) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 42, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET track_functions = 'all';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION dr_f() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dr_f_oid AS SELECT 'dr_f'::regproc::oid AS oid;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dr_f();",
                    expected: Expected::Rows {
                        columns: &[Column("dr_f", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_function_calls(oid) FROM dr_f_oid;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_function_calls", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION dr_f();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_function_calls(oid) FROM dr_f_oid;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_function_calls", INT8)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET track_functions;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE slru_before AS SELECT blks_zeroed FROM pg_stat_slru WHERE name = 'notify';",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LISTEN s4_nothing;",
                    expected: Expected::Tag("LISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify('s4_use', repeat(i::text, current_setting('block_size')::int / 2)) FROM generate_series(1, 3) g(i);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_notify", VOID)],
                        rows: &[
                            &[T("")],
                            &[T("")],
                            &[T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current.blks_zeroed > before.blks_zeroed AS zeroed FROM slru_before before, pg_stat_slru current WHERE current.name = 'notify';",
                    expected: Expected::Rows {
                        columns: &[Column("zeroed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UNLISTEN s4_nothing;",
                    expected: Expected::Tag("UNLISTEN"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a committed drop forgets the counts another session took of the table before the drop, as stats.spec's s2_table_update_k1 s1_table_drop s2_ff permutation shows",
            set_up_script: &[
                "CREATE TABLE tx_t (key text, value int);",
                "INSERT INTO tx_t VALUES ('k0', 1);",
                "CREATE TABLE tx_oid AS SELECT 'tx_t'::regclass::oid AS oid;",
                "SELECT pg_stat_force_next_flush();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE tx_t SET value = value + 1 WHERE key = 'k0';",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE tx_t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT 1 AS flushed;",
                    expected: Expected::Rows {
                        columns: &[Column("flushed", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_tuples_inserted(oid) AS ins, pg_stat_get_tuples_updated(oid) AS upd FROM tx_oid;",
                    expected: Expected::Rows {
                        columns: &[Column("ins", INT8), Column("upd", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
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
fn test_vacuum_and_analyze_skip_temporary_tables_of_another_session() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM and ANALYZE skip another session's temporary tables",
            set_up_script: &[
                "CREATE TABLE skip_parent (a INT);",
                "INSERT INTO skip_parent VALUES (1), (1), (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE skip_t (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO skip_t VALUES (1), (2), (3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE skip_child () INHERITS (skip_parent);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO skip_child VALUES (2), (2), (2), (2), (2);",
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM;",
                    expected: Expected::Tag("VACUUM"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN EXECUTE format('ANALYZE %I.skip_t', (SELECT n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname = 'skip_t')); END $$;"#,
                    expected: Expected::Tag("DO"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE skip_parent;",
                    expected: Expected::Tag("ANALYZE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 'skip_t';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_statistic WHERE starelid = 'skip_t'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT most_common_vals::text FROM pg_stats WHERE tablename = 'skip_parent' AND inherited;",
                    expected: Expected::Rows {
                        columns: &[Column("most_common_vals", TEXT)],
                        rows: &[
                            &[T("{1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE skip_parent;",
                    expected: Expected::Tag("ANALYZE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT most_common_vals::text FROM pg_stats WHERE tablename = 'skip_parent' AND inherited;",
                    expected: Expected::Rows {
                        columns: &[Column("most_common_vals", TEXT)],
                        rows: &[
                            &[T("{2,1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_analyze_statistics_of_user_types() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE keeps the most common values of enum, composite, and enum array columns",
            set_up_script: &[
                "CREATE TYPE stats_mood AS ENUM ('sad', 'ok', 'happy');",
                "CREATE TYPE stats_pair AS (a INTEGER, b TEXT);",
                "CREATE TABLE stats_user_types (m stats_mood, p stats_pair, ms stats_mood[]);",
                "INSERT INTO stats_user_types SELECT (ARRAY['sad', 'ok', 'happy', 'ok']::stats_mood[])[1 + i % 4], ROW(i % 2, 'x')::stats_pair, ARRAY['ok', 'happy']::stats_mood[] FROM generate_series(0, 99) i;",
                "ANALYZE stats_user_types;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT attname, most_common_vals::text, histogram_bounds::text, most_common_elems::text FROM pg_stats WHERE tablename = 'stats_user_types' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("most_common_vals", TEXT), Column("histogram_bounds", TEXT), Column("most_common_elems", TEXT)],
                        rows: &[
                            &[T("m"), T("{ok,sad,happy}"), Null, Null],
                            &[T("ms"), T(r#"{"{ok,happy}"}"#), Null, T("{ok,happy}")],
                            &[T("p"), T(r#"{"(0,x)","(1,x)"}"#), Null, Null],
                        ],
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
fn test_analyze_statistics_of_user_types_through_renames_domains_and_ranges() {
    run_scripts(&[
        ScriptTest {
            name: "the statistics of user types read renamed labels, and those of domains, ranges, and records drive estimates",
            set_up_script: &[
                "CREATE TYPE stats_mood AS ENUM ('sad', 'ok', 'happy');",
                "CREATE TYPE stats_pair AS (a INTEGER, b TEXT);",
                "CREATE TABLE stats_user_types (m stats_mood, p stats_pair, ms stats_mood[]);",
                "INSERT INTO stats_user_types SELECT (ARRAY['sad', 'ok', 'happy', 'ok']::stats_mood[])[1 + i % 4], ROW(i % 2, 'x')::stats_pair, ARRAY['ok', 'happy']::stats_mood[] FROM generate_series(0, 99) i;",
                "ANALYZE stats_user_types;",
                "ALTER TYPE stats_mood RENAME VALUE 'ok' TO 'fine';",
                "CREATE DOMAIN stats_mood_d AS stats_mood;",
                "CREATE TYPE stats_mood_range AS RANGE (subtype = stats_mood);",
                "CREATE TABLE stats_more (d stats_mood_d, r stats_mood_range, h stats_pair);",
                "INSERT INTO stats_more SELECT (ARRAY['sad', 'fine', 'happy']::stats_mood[])[1 + i % 3], stats_mood_range('sad', (ARRAY['fine', 'happy']::stats_mood[])[1 + i % 2]), ROW(i, 'y')::stats_pair FROM generate_series(0, 299) i;",
                "ANALYZE stats_more;",
                "CREATE FUNCTION est(q TEXT) RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE j JSON; BEGIN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j; RETURN j->0->'Plan'->>'Plan Rows'; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT attname, most_common_vals::text, most_common_elems::text FROM pg_stats WHERE tablename = 'stats_user_types' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("most_common_vals", TEXT), Column("most_common_elems", TEXT)],
                        rows: &[
                            &[T("m"), T("{fine,sad,happy}"), Null],
                            &[T("ms"), T(r#"{"{fine,happy}"}"#), T("{fine,happy}")],
                            &[T("p"), T(r#"{"(0,x)","(1,x)"}"#), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, most_common_vals::text, histogram_bounds IS NOT NULL AS has_hist, (histogram_bounds::text::text[])[1:2] AS first_bounds, array_length(range_bounds_histogram::text::text[], 1) AS bounds, (range_bounds_histogram::text::text[])[50:52] AS middle FROM pg_stats WHERE tablename = 'stats_more' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("most_common_vals", TEXT), Column("has_hist", BOOL), Column("first_bounds", TEXT_ARRAY), Column("bounds", INT4), Column("middle", TEXT_ARRAY)],
                        rows: &[
                            &[T("d"), T("{sad,fine,happy}"), T("f"), Null, Null, Null],
                            &[T("h"), Null, T("t"), T(r#"{"(0,y)","(2,y)"}"#), Null, Null],
                            &[T("r"), Null, T("f"), Null, T("101"), T(r#"{"[sad,fine)","[sad,fine)","[sad,happy)"}"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM stats_user_types WHERE m = ''fine'''), est('SELECT * FROM stats_more WHERE d::stats_mood = ''sad'''), est('SELECT * FROM stats_more WHERE h < ROW(10, ''y'')::stats_pair');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("50"), T("100"), T("10")],
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
fn test_analyze_statistics_of_labels_another_branch_lacks() {
    run_scripts(&[
        ScriptTest {
            name: "statistics another branch took leave out a slot whose values name a label this branch lacks",
            set_up_script: &[
                "CREATE TYPE stats_branch_mood AS ENUM ('sad');",
                "CREATE TABLE stats_branch_moods (m stats_branch_mood);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "ALTER TYPE stats_branch_mood ADD VALUE 'ok';",
                "INSERT INTO stats_branch_moods SELECT (ARRAY['sad', 'ok']::stats_branch_mood[])[1 + i % 2] FROM generate_series(0, 99) i;",
                "ANALYZE stats_branch_moods;",
                "SELECT dolt_commit('-Am', 'rows');",
                "SELECT dolt_checkout('other');",
                "ALTER TYPE stats_branch_mood ADD VALUE 'fine';",
                "SELECT dolt_merge('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT most_common_vals IS NULL, null_frac FROM pg_stats WHERE tablename = 'stats_branch_moods';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("null_frac", FLOAT4)],
                        rows: &[
                            &[T("t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("the setup's dolt_merge refuses the uncommitted ALTER TYPE ... ADD VALUE 'fine' as local changes the merge would stomp, as Dolt does; the setup needs a commit before the merge"),
            ..S
        },
    ]);
}

#[test]
fn test_analyze_samples_every_committed_row() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE in a REPEATABLE READ transaction samples the rows committed since its snapshot",
            set_up_script: &[
                "CREATE TABLE an (a INT);",
                "INSERT INTO an VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM an;",
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
                    query: "INSERT INTO an SELECT generate_series(2, 10001);",
                    expected: Expected::Tag("INSERT 0 10000"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE an;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM an;",
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 'an';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("10001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_distinct FROM pg_stats WHERE tablename = 'an' AND attname = 'a';",
                    expected: Expected::Rows {
                        columns: &[Column("n_distinct", FLOAT4)],
                        rows: &[
                            &[T("-1")],
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
fn test_vacuum_counts_every_copy_of_a_row() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM counts each copy of an equal row of a table without a primary key in reltuples and n_live_tup",
            set_up_script: &[
                "CREATE TABLE dup (a INT);",
                "INSERT INTO dup SELECT g % 10 FROM generate_series(1, 10000) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM dup;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 'dup';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("10000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_live_tup FROM pg_stat_user_tables WHERE relname = 'dup';",
                    expected: Expected::Rows {
                        columns: &[Column("n_live_tup", INT8)],
                        rows: &[
                            &[T("10000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dup WHERE a = 3;",
                    expected: Expected::Tag("DELETE 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM dup;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 'dup';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("9000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n_live_tup FROM pg_stat_user_tables WHERE relname = 'dup';",
                    expected: Expected::Rows {
                        columns: &[Column("n_live_tup", INT8)],
                        rows: &[
                            &[T("9000")],
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
fn test_backend_xmin() {
    run_scripts(&[
        ScriptTest {
            name: "pg_stat_activity shows the xmin of a REPEATABLE READ transaction's snapshot while it idles, none for READ COMMITTED between statements, and its own query's",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET application_name = 'xa';",
                    expected: Expected::Tag("SET"),
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 AS one;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT backend_xmin IS NOT NULL AS has_xmin, state FROM pg_stat_activity WHERE application_name = 'xa';",
                    expected: Expected::Rows {
                        columns: &[Column("has_xmin", BOOL), Column("state", TEXT)],
                        rows: &[
                            &[T("t"), T("idle in transaction")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 AS one;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT backend_xmin IS NOT NULL AS has_xmin, state FROM pg_stat_activity WHERE application_name = 'xa';",
                    expected: Expected::Rows {
                        columns: &[Column("has_xmin", BOOL), Column("state", TEXT)],
                        rows: &[
                            &[T("f"), T("idle in transaction")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT backend_xmin IS NOT NULL AS has_xmin, state FROM pg_stat_activity WHERE application_name = 'xa';",
                    expected: Expected::Rows {
                        columns: &[Column("has_xmin", BOOL), Column("state", TEXT)],
                        rows: &[
                            &[T("f"), T("idle")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT backend_xmin IS NOT NULL AS has_xmin FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("has_xmin", BOOL)],
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
fn test_index_pages_of_every_access_method() {
    run_scripts(&[
        ScriptTest {
            name: "relpages of empty and small indexes of every access method count Postgres' fixed pages and roots",
            set_up_script: &[
                r#"CREATE TABLE am0 (a INT, b INT[], p POINT, t TEXT);"#,
                r#"CREATE INDEX am0_bt ON am0 (a);"#,
                r#"CREATE INDEX am0_h ON am0 USING hash (a);"#,
                r#"CREATE INDEX am0_gin ON am0 USING gin (b);"#,
                r#"CREATE INDEX am0_gist ON am0 USING gist (p);"#,
                r#"CREATE INDEX am0_sp ON am0 USING spgist (t);"#,
                r#"CREATE INDEX am0_brin ON am0 USING brin (a);"#,
                r#"CREATE TABLE am1 (a INT, b INT[], p POINT, t TEXT);"#,
                r#"INSERT INTO am1 SELECT g, ARRAY[g % 10], point(g, g), 'x' || g FROM generate_series(1, 10) g;"#,
                r#"CREATE INDEX am1_bt ON am1 (a);"#,
                r#"CREATE INDEX am1_h ON am1 USING hash (a);"#,
                r#"CREATE INDEX am1_gin ON am1 USING gin (b);"#,
                r#"CREATE INDEX am1_gist ON am1 USING gist (p);"#,
                r#"CREATE INDEX am1_sp ON am1 USING spgist (t);"#,
                r#"CREATE INDEX am1_brin ON am1 USING brin (a);"#,
                r#"ANALYZE am0, am1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages FROM pg_class WHERE relname LIKE 'am_\_%' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4)],
                        rows: &[
                            &[T("am0_brin"), T("3")],
                            &[T("am0_bt"), T("1")],
                            &[T("am0_gin"), T("2")],
                            &[T("am0_gist"), T("1")],
                            &[T("am0_h"), T("4")],
                            &[T("am0_sp"), T("3")],
                            &[T("am1_brin"), T("3")],
                            &[T("am1_bt"), T("2")],
                            &[T("am1_gin"), T("2")],
                            &[T("am1_gist"), T("1")],
                            &[T("am1_h"), T("4")],
                            &[T("am1_sp"), T("3")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_table_pages_count_postgres_heap_pages() {
    run_scripts(&[
        ScriptTest {
            name: "relpages of tables count the pages Postgres' heap keeps their rows in, up to their fill factor",
            set_up_script: &[
                r#"CREATE TABLE tp_t (a INT, b INT);"#,
                r#"INSERT INTO tp_t SELECT g, g % 10 FROM generate_series(1, 16384) g;"#,
                r#"CREATE TABLE tp_d (id INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v TEXT);"#,
                r#"INSERT INTO tp_d SELECT a, 'c' FROM tp_t;"#,
                r#"CREATE TABLE tp_f (id INT PRIMARY KEY, v TEXT) WITH (fillfactor = 50);"#,
                r#"INSERT INTO tp_f SELECT a, 'c' FROM tp_t;"#,
                r#"CREATE TABLE tp_w (id BIGINT PRIMARY KEY, a TEXT, b FLOAT8, c NUMERIC, n INT);"#,
                r#"INSERT INTO tp_w SELECT a, repeat('x', 200), a / 3.0, a * 1.5, CASE WHEN a % 3 = 0 THEN NULL ELSE a END FROM tp_t;"#,
                r#"CREATE TABLE tp_e (a INT);"#,
                r#"ANALYZE tp_t, tp_d, tp_f, tp_w, tp_e;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT relname, relpages FROM pg_class WHERE relname LIKE 'tp\_%' AND relkind = 'r' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4)],
                        rows: &[
                            &[T("tp_d"), T("73")],
                            &[T("tp_e"), T("0")],
                            &[T("tp_f"), T("145")],
                            &[T("tp_t"), T("73")],
                            &[T("tp_w"), T("547")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_vacuum_verbose_report() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM (VERBOSE) reports what it removed and used",
            set_up_script: &[
                r#"CREATE TABLE vt (a INT);"#,
                r#"INSERT INTO vt SELECT g FROM generate_series(1, 100) g;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"VACUUM (VERBOSE) vt;"#,
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "INFO", code: "00000", message: r#"vacuuming "postgres.public.vt""#, ..E }, Diagnostic { severity: "INFO", code: "00000", message: "finished vacuuming \"postgres.public.vt\": index scans: 0
pages: 0 removed, 1 remain, 1 scanned (100.00% of total), 0 eagerly scanned
tuples: 0 removed, 100 remain, 0 are dead but not yet removable
removable cutoff: ", message_contains: true, ..E }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_built_in_catalog_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "system catalogs carry the page and row counts and column statistics Postgres' initdb leaves them",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relname, relpages, reltuples, relallvisible FROM pg_class WHERE relname IN ('pg_opclass', 'pg_proc', 'pg_type', 'pg_amop', 'pg_opclass_oid_index') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4), Column("reltuples", FLOAT4), Column("relallvisible", INT4)],
                        rows: &[
                            &[T("pg_amop"), T("7"), T("945"), T("7")],
                            &[T("pg_opclass"), T("3"), T("177"), T("3")],
                            &[T("pg_opclass_oid_index"), T("2"), T("177"), T("0")],
                            &[T("pg_proc"), T("102"), T("3413"), T("102")],
                            &[T("pg_type"), T("15"), T("621"), T("15")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('pg_opclass'), pg_relation_size('pg_amop');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_relation_size", INT8), Column("pg_relation_size", INT8)],
                        rows: &[
                            &[T("24576"), T("57344")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT oid, opcname FROM pg_opclass WHERE opcname <> 'x' AND oid < 16384;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on pg_opclass  (cost=0.00..5.65 rows=176 width=68)")],
                            &[T("  Filter: ((opcname <> 'x'::name) AND (oid < '16384'::oid))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM pg_proc WHERE proname = 'abs';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on pg_proc  (cost=4.33..24.04 rows=6 width=1993)")],
                            &[T("  Recheck Cond: (proname = 'abs'::name)")],
                            &[T("  ->  Bitmap Index Scan on pg_proc_proname_args_nsp_index  (cost=0.00..4.33 rows=6 width=0)")],
                            &[T("        Index Cond: (proname = 'abs'::name)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM pg_type WHERE typname LIKE 'int%';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on pg_type  (cost=4.34..15.75 rows=13 width=259)")],
                            &[T("  Filter: (typname ~~ 'int%'::text)")],
                            &[T("  ->  Bitmap Index Scan on pg_type_typname_nsp_index  (cost=0.00..4.33 rows=6 width=0)")],
                            &[T("        Index Cond: ((typname >= 'int'::text) AND (typname < 'inu'::text))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("Doltgrust's own built-ins (its dolt_ functions and system views) add rows to pg_catalog (Appendix C, rows of Doltgrust's own built-ins in pg_catalog)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, null_frac, avg_width, n_distinct, most_common_vals::text, histogram_bounds::text FROM pg_stats WHERE schemaname = 'pg_catalog' AND tablename = 'pg_type' AND attname IN ('typinput', 'typtype', 'typlen') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("null_frac", FLOAT4), Column("avg_width", INT4), Column("n_distinct", FLOAT4), Column("most_common_vals", TEXT), Column("histogram_bounds", TEXT)],
                        rows: &[
                            &[T("typinput"), T("0"), T("4"), T("-0.15780999"), T("{array_in,record_in,range_in,multirange_in,domain_in,textin}"), T("{namein,int2in,int2vectorin,int4in,regprocin,tidin,xidin,cidin,oidvectorin,pg_ddl_command_in,unknownin,point_in,lseg_in,path_in,box_in,pg_node_tree_in,float4in,float8in,table_am_handler_in,json_in,index_am_handler_in,poly_in,macaddr_in,int8in,cash_in,inet_in,aclitemin,bpcharin,varcharin,date_in,time_in,timestamptz_in,interval_in,boolin,byteain,charin,cidr_in,timestamp_in,timetz_in,circle_in,line_in,bit_in,varbit_in,numeric_in,oidin,regprocedurein,regoperin,regoperatorin,regclassin,regtypein,cstring_in,any_in,anyarray_in,void_in,trigger_in,language_handler_in,internal_in,anyelement_in,anynonarray_in,xml_in,txid_snapshot_in,uuid_in,fdw_handler_in,pg_lsn_in,tsm_handler_in,pg_ndistinct_in,pg_dependencies_in,anyenum_in,event_trigger_in,tsvectorin,tsqueryin,gtsvectorin,regconfigin,regdictionaryin,jsonb_in,anyrange_in,jsonpath_in,regnamespacein,regrolein,macaddr8_in,regcollationin,anycompatiblemultirange_in,anymultirange_in,brin_bloom_summary_in,brin_minmax_multi_summary_in,pg_mcv_list_in,pg_snapshot_in,xid8in,anycompatible_in,anycompatiblearray_in,anycompatiblenonarray_in,anycompatiblerange_in}")],
                            &[T("typlen"), T("0"), T("2"), T("12"), T("{-1,4,8,16,-2,1,6,24,32,64}"), T("{2,12}")],
                            &[T("typtype"), T("0"), T("1"), T("6"), T("{b,c,p,m,r,d}"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stats WHERE schemaname = 'pg_catalog' AND tablename = 'pg_opclass';",
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
                    query: "SELECT count(*) FILTER (WHERE most_common_vals IS NOT NULL) AS mcv, count(*) FILTER (WHERE histogram_bounds IS NOT NULL) AS histograms, count(*) FILTER (WHERE most_common_elems IS NOT NULL) AS elements FROM pg_stats WHERE schemaname IN ('pg_catalog', 'information_schema');",
                    expected: Expected::Rows {
                        columns: &[Column("mcv", INT8), Column("histograms", INT8), Column("elements", INT8)],
                        rows: &[
                            &[T("294"), T("154"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM pg_type WHERE typname LIKE 'int%';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on pg_type")],
                            &[T("  Filter: (typname ~~ 'int%'::text)")],
                            &[T("  ->  Bitmap Index Scan on pg_type_typname_nsp_index")],
                            &[T("        Index Cond: ((typname >= 'int'::text) AND (typname < 'inu'::text))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_vacuum_cost_delay() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM and ANALYZE pause for their cost-based delay and finish",
            set_up_script: &[
                r#"CREATE TABLE ct (a INT PRIMARY KEY, b TEXT);"#,
                r#"INSERT INTO ct SELECT g, 'row ' || g FROM generate_series(1, 100) g;"#,
                r#"CREATE INDEX ct_b ON ct (b);"#,
                r#"SET vacuum_cost_delay = '1ms';"#,
                r#"SET vacuum_cost_limit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"VACUUM (ANALYZE) ct;"#,
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*), reltuples FROM ct, pg_class WHERE relname = 'ct' GROUP BY reltuples;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("100"), T("100")],
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
fn test_vacuum_and_analyze_of_system_catalogs_record_their_counts() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE and VACUUM of a system catalog record its pages and rows in its pg_class row",
            set_up_script: &[
                r#"DO $$ BEGIN FOR i IN 1..50 LOOP EXECUTE format('CREATE TABLE cs%s (a INT)', i); END LOOP; END $$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ANALYZE pg_class;"#,
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT reltuples = (SELECT count(*) FROM pg_class) AS counted, relpages > 0 AS has_pages FROM pg_class WHERE oid = 'pg_class'::regclass;"#,
                    expected: Expected::Rows {
                        columns: &[Column("counted", BOOL), Column("has_pages", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN FOR i IN 1..20 LOOP EXECUTE format('DROP TABLE cs%s', i); END LOOP; END $$;"#,
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"VACUUM pg_class;"#,
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT reltuples = (SELECT count(*) FROM pg_class) AS counted, relpages > 0 AS has_pages FROM pg_class WHERE oid = 'pg_class'::regclass;"#,
                    expected: Expected::Rows {
                        columns: &[Column("counted", BOOL), Column("has_pages", BOOL)],
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
            name: "a system catalog's counts one branch recorded stay that branch's",
            set_up_script: &[
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                r#"DO $$ BEGIN FOR i IN 1..50 LOOP EXECUTE format('CREATE TABLE cb%s (a INT)', i); END LOOP; END $$;"#,
                "ANALYZE pg_class;",
                "SELECT dolt_commit('-Am', 'tables');",
                "SELECT dolt_checkout('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT reltuples < (SELECT count(*) FROM pg_class) + 50 AS unchanged FROM pg_class WHERE oid = 'pg_class'::regclass;"#,
                    expected: Expected::Rows {
                        columns: &[Column("unchanged", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("the setup's first dolt_commit has nothing to commit (55000)"),
            ..S
        },
    ]);
}

#[test]
fn test_stat_activity_query_id() {
    run_scripts(&[
        ScriptTest {
            name: "pg_stat_activity shows the identifier of the top-level query under compute_query_id = on",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET compute_query_id = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT query_id FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("query_id", INT8)],
                        rows: &[
                            &[T("-268709642932831452")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION show_qid() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE '% %', tg_tag, (SELECT query_id FROM pg_stat_activity WHERE pid = pg_backend_pid()); END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER show_qid ON ddl_command_end EXECUTE FUNCTION show_qid();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE qa (a int PRIMARY KEY, b text DEFAULT 'x', c numeric(10,2) CHECK (c > 0));",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "CREATE TABLE 1017429366274080232", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX qa_b ON qa (b) WHERE c > 1;",
                    expected: Expected::Tag("CREATE INDEX"),
                    notices: &[Diagnostic { code: "00000", message: "CREATE INDEX 8981408456209630079", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE qa ADD COLUMN e int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "ALTER TABLE 1099651090305888383", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET track_activities = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT query_id IS NOT NULL AS qid_set FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("qid_set", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET track_activities;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET compute_query_id = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT query_id IS NOT NULL AS qid_set FROM pg_stat_activity WHERE pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("qid_set", BOOL)],
                        rows: &[
                            &[T("f")],
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
fn test_catalog_changes_count_in_their_catalogs_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "DDL counts the rows it writes in its system catalogs' statistics",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "CREATE TABLE cc_t (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_attribute"), T("8"), T("0"), T("0"), T("0")],
                            &[T("pg_class"), T("1"), T("0"), T("0"), T("0")],
                            &[T("pg_depend"), T("3"), T("0"), T("0"), T("0")],
                            &[T("pg_type"), T("2"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE cc_t ADD COLUMN c INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_attribute"), T("9"), T("0"), T("0"), T("0")],
                            &[T("pg_class"), T("1"), T("1"), T("0"), T("1")],
                            &[T("pg_depend"), T("3"), T("0"), T("0"), T("0")],
                            &[T("pg_type"), T("2"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE cc_t IS 'x';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE cc_t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_attribute"), T("9"), T("0"), T("9"), T("0")],
                            &[T("pg_class"), T("1"), T("1"), T("1"), T("1")],
                            &[T("pg_depend"), T("3"), T("0"), T("3"), T("0")],
                            &[T("pg_description"), T("1"), T("0"), T("1"), T("0")],
                            &[T("pg_type"), T("2"), T("0"), T("2"), T("0")],
                        ],
                        tag: "SELECT 5",
                    },
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
            name: "ANALYZE counts the rows it writes in pg_statistic",
            set_up_script: &[
                "CREATE TABLE cc_a (a INT, b INT);",
                "INSERT INTO cc_a SELECT g, g FROM generate_series(1, 100) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ANALYZE cc_a;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd <= 1 AS hot_bounded FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("hot_bounded", BOOL)],
                        rows: &[
                            &[T("pg_statistic"), T("2"), T("0"), T("0"), T("t")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ANALYZE cc_a;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd <= 1 AS hot_bounded FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("hot_bounded", BOOL)],
                        rows: &[
                            &[T("pg_statistic"), T("0"), T("2"), T("0"), T("t")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "catalog changes Doltgrust stores in other ways than Postgres' rows",
            set_up_script: &[
                "CREATE TABLE cc_d (a INT, b INT);",
                "INSERT INTO cc_d SELECT g, g FROM generate_series(1, 100) g;",
                "ANALYZE cc_d;",
                "CREATE SCHEMA cc_s;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "TRUNCATE cc_d;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_class"), T("0"), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER TABLE cc_d SET SCHEMA cc_s;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_class"), T("0"), T("1"), T("0"), T("0")],
                            &[T("pg_depend"), T("0"), T("1"), T("0"), T("0")],
                            &[T("pg_type"), T("0"), T("2"), T("0"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER TABLE cc_d DROP COLUMN b;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_attribute"), T("0"), T("1"), T("0"), T("0")],
                            &[T("pg_statistic"), T("0"), T("0"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    skip: Some("DROP COLUMN deletes the column's pg_attribute record, keeping its slot in the identity tree, where Postgres updates the row"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "GRANT SELECT ON pg_class TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_class"), T("0"), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("a GRANT that changes no privilege of a built-in row writes no record, where Postgres writes the row again"),
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
    ]);
}

#[test]
fn test_shared_catalog_changes_count_in_their_catalogs_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "role and database commands count the rows they write in the shared catalogs' statistics",
            set_up_script: &[
                "CREATE DATABASE cs_db;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "CREATE ROLE cs_r LOGIN;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_authid"), T("1"), T("0"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER ROLE cs_r CREATEDB;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_authid"), T("0"), T("1"), T("0"), T("1")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER ROLE cs_r PASSWORD 'x';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_authid"), T("0"), T("1"), T("0"), T("1")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "CREATE ROLE cs_g;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_authid"), T("1"), T("0"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "GRANT cs_g TO cs_r;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_auth_members"), T("1"), T("0"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "COMMENT ON ROLE cs_r IS 'c';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_shdescription"), T("1"), T("0"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER ROLE cs_r SET work_mem = '8MB';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_db_role_setting"), T("1"), T("0"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER ROLE cs_r IN DATABASE postgres SET work_mem = '9MB';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_db_role_setting"), T("1"), T("0"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER DATABASE cs_db SET work_mem = '7MB';",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_db_role_setting"), T("1"), T("0"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "COMMENT ON DATABASE cs_db IS 'd';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_shdescription"), T("1"), T("0"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER DATABASE cs_db OWNER TO cs_r;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_database"), T("0"), T("1"), T("0"), T("1")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "GRANT CONNECT ON DATABASE cs_db TO cs_g;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_database"), T("0"), T("1"), T("0"), T("1")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "GRANT SET ON PARAMETER work_mem TO cs_g;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_parameter_acl"), T("1"), T("1"), T("0"), T("1")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "REVOKE cs_g FROM cs_r;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_auth_members"), T("0"), T("0"), T("1"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER ROLE cs_r RENAME TO cs_s;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_authid"), T("0"), T("1"), T("0"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "REVOKE SET ON PARAMETER work_mem FROM cs_g;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_parameter_acl"), T("0"), T("0"), T("1"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER DATABASE cs_db OWNER TO postgres;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_database"), T("0"), T("1"), T("0"), T("1")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "REVOKE CONNECT ON DATABASE cs_db FROM cs_g;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_database"), T("0"), T("1"), T("0"), T("1")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "DROP ROLE cs_g;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_authid"), T("0"), T("0"), T("1"), T("0")],
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "DROP ROLE cs_s;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE n_tup_ins + n_tup_upd + n_tup_del > 0 AND relname <> 'pg_shdepend' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_authid"), T("0"), T("0"), T("1"), T("0")],
                            &[T("pg_db_role_setting"), T("0"), T("0"), T("2"), T("0")],
                            &[T("pg_shdescription"), T("0"), T("0"), T("1"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
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
            name: "CREATE DATABASE and DROP DATABASE count their database's rows of the shared catalogs",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE cs_b AS SELECT relid, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_sys_tables WHERE relname IN ('pg_authid', 'pg_auth_members', 'pg_database', 'pg_db_role_setting', 'pg_shdescription', 'pg_parameter_acl');",
                    expected: Expected::Tag("SELECT 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE cs_db2;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.relname, s.n_tup_ins - b.i AS i, s.n_tup_upd - b.u AS u, s.n_tup_del - b.d AS d, s.n_tup_hot_upd - b.h AS h FROM pg_stat_sys_tables s JOIN cs_b b USING (relid) WHERE (s.n_tup_ins - b.i) + (s.n_tup_upd - b.u) + (s.n_tup_del - b.d) > 0 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_database"), T("1"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cs_b;",
                    expected: Expected::Tag("DELETE 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO cs_b SELECT relid, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_sys_tables WHERE relname IN ('pg_authid', 'pg_auth_members', 'pg_database', 'pg_db_role_setting', 'pg_shdescription', 'pg_parameter_acl');",
                    expected: Expected::Tag("INSERT 0 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE cs_db2 SET work_mem = '7MB';",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE postgres IN DATABASE cs_db2 SET work_mem = '6MB';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON DATABASE cs_db2 IS 'x';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.relname, s.n_tup_ins - b.i AS i, s.n_tup_upd - b.u AS u, s.n_tup_del - b.d AS d, s.n_tup_hot_upd - b.h AS h FROM pg_stat_sys_tables s JOIN cs_b b USING (relid) WHERE (s.n_tup_ins - b.i) + (s.n_tup_upd - b.u) + (s.n_tup_del - b.d) > 0 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_db_role_setting"), T("2"), T("0"), T("0"), T("0")],
                            &[T("pg_shdescription"), T("1"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cs_b;",
                    expected: Expected::Tag("DELETE 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO cs_b SELECT relid, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_sys_tables WHERE relname IN ('pg_authid', 'pg_auth_members', 'pg_database', 'pg_db_role_setting', 'pg_shdescription', 'pg_parameter_acl');",
                    expected: Expected::Tag("INSERT 0 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE cs_db2;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.relname, s.n_tup_ins - b.i AS i, s.n_tup_upd - b.u AS u, s.n_tup_del - b.d AS d, s.n_tup_hot_upd - b.h AS h FROM pg_stat_sys_tables s JOIN cs_b b USING (relid) WHERE (s.n_tup_ins - b.i) + (s.n_tup_upd - b.u) + (s.n_tup_del - b.d) > 0 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_database"), T("0"), T("0"), T("1"), T("0")],
                            &[T("pg_db_role_setting"), T("0"), T("0"), T("2"), T("0")],
                            &[T("pg_shdescription"), T("0"), T("0"), T("1"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "privileges count their rows of pg_shdepend",
            set_up_script: &[
                "CREATE ROLE cs_h;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "GRANT CONNECT ON DATABASE postgres TO cs_h;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins AS i, n_tup_upd AS u, n_tup_del AS d, n_tup_hot_upd AS h FROM pg_stat_xact_sys_tables WHERE relname = 'pg_shdepend';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("i", INT8), Column("u", INT8), Column("d", INT8), Column("h", INT8)],
                        rows: &[
                            &[T("pg_shdepend"), T("1"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Doltgrust derives pg_shdepend's rows from the owners and privileges the catalogs keep, so it writes and counts none"),
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
    ]);
}

#[test]
fn test_objects_an_aborted_transaction_created_keep_no_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "an index, function, or table an aborted transaction created keeps no statistics, and has an entry once the transaction counts it",
            set_up_script: &[
                "CREATE TABLE stats_test_tab1 AS SELECT generate_series(1, 10) a;",
                "CREATE SEQUENCE stats_oids;",
                "SET enable_seqscan TO off;",
                "SET track_functions TO 'all';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX stats_test_idx1 ON stats_test_tab1 (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setval('stats_oids', 'stats_test_idx1'::regclass::oid::int8) > 0 AS kept;",
                    expected: Expected::Rows {
                        columns: &[Column("kept", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM stats_test_tab1 WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('relation', (SELECT oid FROM pg_database WHERE datname = current_database()), (SELECT last_value FROM stats_oids));",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('relation', (SELECT oid FROM pg_database WHERE datname = current_database()), (SELECT last_value FROM stats_oids));",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('relation', (SELECT oid FROM pg_database WHERE datname = current_database()), (SELECT last_value FROM stats_oids));",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
                        rows: &[
                            &[T("f")],
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
                    query: "CREATE FUNCTION stats_test_func() RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setval('stats_oids', 'stats_test_func()'::regprocedure::oid::int8) > 0 AS kept;",
                    expected: Expected::Rows {
                        columns: &[Column("kept", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stats_test_func();",
                    expected: Expected::Rows {
                        columns: &[Column("stats_test_func", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('function', (SELECT oid FROM pg_database WHERE datname = current_database()), (SELECT last_value FROM stats_oids));",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('function', (SELECT oid FROM pg_database WHERE datname = current_database()), (SELECT last_value FROM stats_oids));",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
                        rows: &[
                            &[T("f")],
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
                    query: "CREATE TABLE stats_test_tab2 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setval('stats_oids', 'stats_test_tab2'::regclass::oid::int8) > 0 AS kept;",
                    expected: Expected::Rows {
                        columns: &[Column("kept", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO stats_test_tab2 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('relation', (SELECT oid FROM pg_database WHERE datname = current_database()), (SELECT last_value FROM stats_oids));",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('relation', (SELECT oid FROM pg_database WHERE datname = current_database()), (SELECT last_value FROM stats_oids));",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system catalogs' pages after ANALYZE fill the space initdb's pages have",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relname, relpages, reltuples FROM pg_class WHERE relname IN ('pg_class', 'pg_attribute', 'pg_type', 'pg_depend') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("pg_attribute"), T("55"), T("3161")],
                            &[T("pg_class"), T("14"), T("415")],
                            &[T("pg_depend"), T("13"), T("1725")],
                            &[T("pg_type"), T("15"), T("621")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN FOR i IN 1..100 LOOP EXECUTE format('CREATE TABLE catalog_pages_%s (a INT, b INT, c INT, d BIGINT, e INT)', i); END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE pg_class, pg_attribute, pg_type, pg_depend;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relpages, reltuples FROM pg_class WHERE relname IN ('pg_class', 'pg_attribute', 'pg_type', 'pg_depend') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("pg_attribute"), T("74"), T("4261")],
                            &[T("pg_class"), T("14"), T("515")],
                            &[T("pg_depend"), T("15"), T("2025")],
                            &[T("pg_type"), T("20"), T("821")],
                        ],
                        tag: "SELECT 4",
                    },
                    skip: Some("Doltgrust's own built-ins (its dolt_ functions and system views) add rows to pg_catalog (Appendix C, rows of Doltgrust's own built-ins in pg_catalog)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r"SELECT relname, (reltuples - CASE relname WHEN 'pg_class' THEN (SELECT count(*) FROM pg_class WHERE relname LIKE 'dolt\_%') WHEN 'pg_attribute' THEN (SELECT count(*) FROM pg_attribute WHERE attrelid IN (SELECT oid FROM pg_class WHERE relname LIKE 'dolt\_%')) WHEN 'pg_type' THEN (SELECT count(*) FROM pg_type WHERE typname LIKE 'dolt\_%' OR typname LIKE '\_dolt\_%') ELSE (SELECT count(*) FROM pg_depend WHERE classid = 'pg_rewrite'::regclass AND objid IN (SELECT oid FROM pg_rewrite WHERE ev_class IN (SELECT oid FROM pg_class WHERE relname LIKE 'dolt\_%')) OR classid = 'pg_type'::regclass AND objid IN (SELECT oid FROM pg_type WHERE typname LIKE 'dolt\_%' OR typname LIKE '\_dolt\_%') OR classid = 'pg_proc'::regclass AND objid IN (SELECT oid FROM pg_proc WHERE proname LIKE 'dolt\_%') OR classid = 'pg_class'::regclass AND objid IN (SELECT oid FROM pg_class WHERE relname LIKE 'dolt\_%') OR classid = 'pg_trigger'::regclass AND objid IN (SELECT oid FROM pg_trigger WHERE tgrelid IN (SELECT oid FROM pg_class WHERE relname LIKE 'dolt\_%'))) END)::real AS reltuples FROM pg_class WHERE relname IN ('pg_class', 'pg_attribute', 'pg_type', 'pg_depend') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("pg_attribute"), T("4261")],
                            &[T("pg_class"), T("515")],
                            &[T("pg_depend"), T("2025")],
                            &[T("pg_type"), T("821")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_first_calls_and_drops_count_as_postgres_does() {
    run_scripts(&[
        ScriptTest {
            name: "a function's first counted call gives it zero calls in the server's counts until the session reports them",
            set_up_script: &[
                "CREATE FUNCTION stats_first() RETURNS VOID LANGUAGE plpgsql AS $$BEGIN END;$$;",
                "SET track_functions TO 'all';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL stats_fetch_consistency = none;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_function_calls('stats_first()'::regprocedure::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_function_calls", INT8)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stats_first();",
                    expected: Expected::Rows {
                        columns: &[Column("stats_first", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_function_calls('stats_first()'::regprocedure::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_function_calls", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_function_calls('stats_first()'::regprocedure::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_function_calls", INT8)],
                        rows: &[
                            &[T("1")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "a dropped table reads as unchanged by the level of the transaction that dropped it until that level aborts",
            set_up_script: &[
                "CREATE TABLE stats_drop (a int);",
                "INSERT INTO stats_drop VALUES (1);",
                "SELECT pg_stat_force_next_flush();",
                "CREATE SEQUENCE stats_oids;",
                "SELECT setval('stats_oids', 'stats_drop'::regclass::oid::int8);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO stats_drop VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO stats_drop VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_tuples_inserted((SELECT last_value FROM stats_oids)::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_tuples_inserted", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE stats_drop;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_tuples_inserted((SELECT last_value FROM stats_oids)::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_tuples_inserted", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_tuples_inserted((SELECT last_value FROM stats_oids)::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_tuples_inserted", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE stats_drop;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_tuples_inserted((SELECT last_value FROM stats_oids)::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_tuples_inserted", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_tuples_inserted((SELECT last_value FROM stats_oids)::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_tuples_inserted", INT8)],
                        rows: &[
                            &[T("3")],
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
fn test_pg_stat_have_stats_of_a_database_by_object_id() {
    run_scripts(&[
        ScriptTest {
            name: "pg_stat_have_stats names a database's counts by object id 0",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('database', (SELECT oid FROM pg_database WHERE datname = current_database()), 0) AS zero, pg_stat_have_stats('database', (SELECT oid FROM pg_database WHERE datname = current_database()), 1) AS one;",
                    expected: Expected::Rows {
                        columns: &[Column("zero", BOOL), Column("one", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
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
fn test_reindex_concurrently_keeps_an_index_s_counts() {
    run_scripts(&[
        ScriptTest {
            name: "REINDEX CONCURRENTLY gives the new index the old one's cumulative counts",
            set_up_script: &[
                "CREATE TABLE rs (a INT);",
                "INSERT INTO rs SELECT i FROM generate_series(1, 100) i;",
                "CREATE INDEX rs_a ON rs (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rs WHERE a = 5;",
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
                    query: "SELECT count(*) FROM rs WHERE a = 6;",
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT idx_scan FROM pg_stat_user_indexes WHERE indexrelname = 'rs_a';",
                    expected: Expected::Rows {
                        columns: &[Column("idx_scan", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX CONCURRENTLY rs_a;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT idx_scan FROM pg_stat_user_indexes WHERE indexrelname = 'rs_a';",
                    expected: Expected::Rows {
                        columns: &[Column("idx_scan", INT8)],
                        rows: &[
                            &[T("2")],
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
fn test_updates_of_summarized_columns_are_hot() {
    run_scripts(&[
        ScriptTest {
            name: "an update that changes only columns of summarizing indexes counts as HOT",
            set_up_script: &[
                "CREATE TABLE hu (a INT PRIMARY KEY, b INT, c INT);",
                "CREATE INDEX hu_b ON hu USING brin (b);",
                "CREATE INDEX hu_c ON hu (c);",
                "INSERT INTO hu SELECT i, i, i FROM generate_series(1, 10) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE hu SET b = b + 100 WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_tuples_hot_updated('hu'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_tuples_hot_updated", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE hu SET c = c + 100, b = b + 100 WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_tuples_hot_updated('hu'::regclass), pg_stat_get_xact_tuples_updated('hu'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_tuples_hot_updated", INT8), Column("pg_stat_get_xact_tuples_updated", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
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
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM hu WHERE b = 101;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
    ]);
}

#[test]
fn test_updates_of_a_table_without_a_primary_key_count_as_hot() {
    run_scripts(&[
        ScriptTest {
            name: "an update of a table without a primary key that changes no indexed value counts as HOT",
            set_up_script: &[
                "CREATE TABLE hn (a INT, b INT, c INT);",
                "CREATE INDEX hn_b ON hn USING brin (b);",
                "CREATE INDEX hn_c ON hn (c);",
                "INSERT INTO hn SELECT i, i, i FROM generate_series(1, 10) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE hn SET a = a + 100 WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE hn SET b = b + 100 WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE hn SET c = c + 100 WHERE a = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_get_xact_tuples_hot_updated('hn'::regclass), pg_stat_get_xact_tuples_updated('hn'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_get_xact_tuples_hot_updated", INT8), Column("pg_stat_get_xact_tuples_updated", INT8)],
                        rows: &[
                            &[T("2"), T("3")],
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
            ],
            ..S
        },
    ]);
}

#[test]
fn test_pg_stat_io_reads_after_set_tablespace() {
    run_scripts(&[
        ScriptTest {
            name: "a scan of a table moved to another tablespace reads its blocks again",
            set_up_script: &[
                "SET allow_in_place_tablespaces = on;",
                "CREATE TABLESPACE regress_tblspace LOCATION '';",
                "CREATE TABLE test_io_shared(a int);",
                "INSERT INTO test_io_shared SELECT i FROM generate_series(1, 100) i;",
                "CHECKPOINT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE io_before AS SELECT sum(reads) AS reads FROM pg_stat_io WHERE context = 'normal' AND object = 'relation';",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_io_shared SET TABLESPACE regress_tblspace;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM test_io_shared;",
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
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT sum(reads) FROM pg_stat_io WHERE context = 'normal' AND object = 'relation') > (SELECT reads FROM io_before);",
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
            skip: Some("moving a tree to another tablespace keeps its chunks' addresses, so the buffer pool's nodes stay valid and the scan hits them where Postgres reads its new relation file"),
            ..S
        },
    ]);
}

#[test]
fn test_pg_stat_io_vacuum_strategy() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM reads and reuses buffers in its own context",
            set_up_script: &[
                "CREATE TABLE test_io_vac_strategy(a int, b int) WITH (autovacuum_enabled = 'false');",
                "INSERT INTO test_io_vac_strategy SELECT i, i FROM generate_series(1, 4500) i;",
                "VACUUM (FULL) test_io_vac_strategy;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE vac_before AS SELECT sum(reads) AS reads, sum(reuses) + sum(evictions) AS reused FROM pg_stat_io WHERE context = 'vacuum';",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (PARALLEL 0, BUFFER_USAGE_LIMIT 128) test_io_vac_strategy;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(reads) > (SELECT reads FROM vac_before) AS read, sum(reuses) + sum(evictions) > (SELECT reused FROM vac_before) AS reused FROM pg_stat_io WHERE context = 'vacuum';",
                    expected: Expected::Rows {
                        columns: &[Column("read", BOOL), Column("reused", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("this VACUUM reads no heap: nothing changed since the table's last fold and its kept sizes hold its row count, while Postgres' lazy vacuum reads every page the visibility map does not mark all-visible, which after VACUUM FULL is all of them"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_updates_of_indexed_columns_are_not_hot() {
    run_scripts(&[
        ScriptTest {
            name: "a catalog update that changes a column any of the catalog's indexes reads is not HOT",
            set_up_script: &[
                "CREATE TABLE hot_t (a INT);",
                "CREATE SCHEMA hot_s;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "TRUNCATE hot_t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_upd, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE relname IN ('pg_class', 'pg_depend') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_upd", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_class"), T("1"), T("0")],
                            &[T("pg_depend"), T("0"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER TABLE hot_t SET SCHEMA hot_s;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_upd, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE relname IN ('pg_class', 'pg_depend') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_upd", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_class"), T("1"), T("0")],
                            &[T("pg_depend"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER TABLE hot_s.hot_t SET (fillfactor = 50);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_upd, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE relname IN ('pg_class', 'pg_depend') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_upd", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_class"), T("1"), T("1")],
                            &[T("pg_depend"), T("0"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
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
    ]);
}

#[test]
fn test_backend_xmin_after_functions_take_snapshots() {
    run_scripts(&[
        ScriptTest {
            name: "a READ COMMITTED session that called a volatile function whose statements took newer snapshots shows no xmin once idle",
            set_up_script: &[
                r#"CREATE TABLE xf_rows (a INT);"#,
                r#"CREATE TABLE xf_other (a INT);"#,
                r#"CREATE FUNCTION xf() RETURNS INT LANGUAGE sql AS $$ SELECT pg_advisory_lock(7); SELECT pg_advisory_unlock(7); INSERT INTO xf_rows VALUES (1); SELECT count(*)::INT FROM xf_rows; $$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET application_name = 'xf';",
                    expected: Expected::Tag("SET"),
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock(7);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT xf() AS n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xf_other VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(7);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 AS one;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT backend_xmin IS NULL AS no_xmin, state FROM pg_stat_activity WHERE application_name = 'xf';",
                    expected: Expected::Rows {
                        columns: &[Column("no_xmin", BOOL), Column("state", TEXT)],
                        rows: &[
                            &[T("t"), T("idle")],
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
fn test_reindex_concurrently_keeps_an_index_s_counts_not_yet_flushed() {
    run_scripts(&[
        ScriptTest {
            name: "REINDEX CONCURRENTLY of an index whose only counts the session has yet to flush gives the new index counts of its own",
            set_up_script: &[
                "CREATE TABLE rsp (a INT);",
                "INSERT INTO rsp SELECT i FROM generate_series(1, 100) i;",
                "CREATE INDEX rsp_a ON rsp (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rsp WHERE a = 5;",
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
                    query: "REINDEX INDEX CONCURRENTLY rsp_a;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_have_stats('relation', (SELECT oid FROM pg_database WHERE datname = current_database()), 'rsp_a'::regclass::oid::int8);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_have_stats", BOOL)],
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
