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
fn test_cluster() {
    run_scripts(&[
        ScriptTest {
            name: "CLUSTER and VACUUM FULL give a table and its indexes new relfilenodes and Postgres' counts",
            set_up_script: &[
                "CREATE TABLE cl (id INT PRIMARY KEY, b INT, t TEXT);",
                "CREATE INDEX cl_b ON cl (b);",
                "CREATE INDEX cl_p ON cl (b) WHERE b < 3;",
                "INSERT INTO cl SELECT g, 10 - g % 10, 'x' || g FROM generate_series(1, 1000) g;",
                "CREATE TABLE g (a INT[]);",
                "CREATE INDEX g_gin ON g USING gin (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CLUSTER cl;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"there is no previously clustered index for table "cl""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER cl USING nope;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"index "nope" for table "cl" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER cl USING cl_p;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot cluster on partial index "cl_p""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER g USING g_gin;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot cluster on index "g_gin" because access method does not support clustering"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER (FOO) cl;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized CLUSTER option "foo""#, position: 10, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE before AS SELECT relname, relfilenode FROM pg_class WHERE relname LIKE 'cl%';",
                    expected: Expected::Tag("SELECT 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER VERBOSE cl USING cl_b;",
                    expected: Expected::Tag("CLUSTER"),
                    notices: &[Diagnostic { severity: "INFO", code: "00000", message: r#"clustering "public.cl" using index scan on "cl_b""#, ..N }, Diagnostic { severity: "INFO", code: "00000", message: r#""public.cl": found 0 removable, 1000 nonremovable row versions in "#, message_contains: true, detail: "0 dead row versions cannot be removed yet.\nCPU: user: ", detail_contains: true, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass::text, indisclustered FROM pg_index WHERE indrelid = 'cl'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", TEXT), Column("indisclustered", BOOL)],
                        rows: &[
                            &[T("cl_b"), T("t")],
                            &[T("cl_p"), T("f")],
                            &[T("cl_pkey"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relfilenode = b.relfilenode AS same FROM pg_class c JOIN before b USING (relname) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("same", BOOL)],
                        rows: &[
                            &[T("cl"), T("f")],
                            &[T("cl_b"), T("f")],
                            &[T("cl_p"), T("f")],
                            &[T("cl_pkey"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltuples FROM pg_class WHERE relname LIKE 'cl%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("cl"), T("1000")],
                            &[T("cl_b"), T("1000")],
                            &[T("cl_p"), T("200")],
                            &[T("cl_pkey"), T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(b) FROM cl WHERE b < 5;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("400"), T("1000")],
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
                    query: "CLUSTER cl;",
                    expected: Expected::Tag("CLUSTER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CLUSTER cannot run inside a transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER;",
                    expected: Expected::Tag("CLUSTER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CLUSTER cl; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CLUSTER; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CLUSTER cannot be executed from a function", context: Some("SQL statement \"CLUSTER\"\nPL/pgSQL function inline_code_block line 1 at SQL statement"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE before;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE before AS SELECT relname, relfilenode FROM pg_class WHERE relname LIKE 'cl%';",
                    expected: Expected::Tag("SELECT 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM FULL VERBOSE cl;",
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "INFO", code: "00000", message: r#"vacuuming "public.cl""#, ..N }, Diagnostic { severity: "INFO", code: "00000", message: r#""public.cl": found 0 removable, 1000 nonremovable row versions in "#, message_contains: true, detail: "0 dead row versions cannot be removed yet.\nCPU: user: ", detail_contains: true, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relfilenode = b.relfilenode AS same FROM pg_class c JOIN before b USING (relname) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("same", BOOL)],
                        rows: &[
                            &[T("cl"), T("f")],
                            &[T("cl_b"), T("f")],
                            &[T("cl_p"), T("f")],
                            &[T("cl_pkey"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass::text FROM pg_index WHERE indrelid = 'cl'::regclass AND indisclustered;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", TEXT)],
                        rows: &[
                            &[T("cl_b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CLUSTER of a partitioned table clusters each partition on its index",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b INT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (100);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (100) TO (200);",
                "CREATE INDEX pt_b ON pt (b);",
                "INSERT INTO pt SELECT g, g FROM generate_series(0, 199) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE before AS SELECT relname, relfilenode FROM pg_class WHERE relname LIKE 'pt%';",
                    expected: Expected::Tag("SELECT 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER pt USING pt_b;",
                    expected: Expected::Tag("CLUSTER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relfilenode = b.relfilenode AS same FROM pg_class c JOIN before b USING (relname) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("same", BOOL)],
                        rows: &[
                            &[T("pt"), T("t")],
                            &[T("pt1"), T("f")],
                            &[T("pt1_b_idx"), T("f")],
                            &[T("pt2"), T("f")],
                            &[T("pt2_b_idx"), T("f")],
                            &[T("pt_b"), T("t")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass::text, indisclustered FROM pg_index WHERE indrelid IN ('pt'::regclass, 'pt1'::regclass, 'pt2'::regclass) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", TEXT), Column("indisclustered", BOOL)],
                        rows: &[
                            &[T("pt1_b_idx"), T("t")],
                            &[T("pt2_b_idx"), T("t")],
                            &[T("pt_b"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER pt;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"there is no previously clustered index for table "pt""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt CLUSTER ON pt_b;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot mark index clustered in partitioned table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER pt USING pt_b;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CLUSTER cannot run inside a transaction block", ..E }),
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
            name: "CLUSTER skips the tables the current user may not maintain",
            set_up_script: &[
                "CREATE ROLE clu_user;",
                "CREATE TABLE big (id INT PRIMARY KEY);",
                "ALTER TABLE big CLUSTER ON big_pkey;",
                "CREATE TABLE mine (a INT PRIMARY KEY);",
                "ALTER TABLE mine OWNER TO clu_user;",
                "ALTER TABLE mine CLUSTER ON mine_pkey;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET ROLE clu_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER;",
                    expected: Expected::Tag("CLUSTER"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"permission denied to cluster "big", skipping it"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER big;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table big", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass::text FROM pg_index WHERE indisclustered ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", TEXT)],
                        rows: &[
                            &[T("big_pkey")],
                            &[T("mine_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CLUSTER and VACUUM FULL keep every row an older snapshot sees",
            set_up_script: &[
                "CREATE TABLE o (x INT);",
                "CREATE TABLE d (id INT PRIMARY KEY, b INT);",
                "INSERT INTO d SELECT g, g % 10 FROM generate_series(1, 1000) g;",
                "CREATE INDEX d_b ON d (b);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM o;",
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
                    query: "/* client B */ DELETE FROM d WHERE b = 1 AND id < 500;",
                    expected: Expected::Tag("DELETE 50"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO d VALUES (5001, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CLUSTER d USING d_b;",
                    expected: Expected::Tag("CLUSTER"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ VACUUM FULL d;",
                    expected: Expected::Tag("VACUUM"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*), sum(id) FROM d WHERE b = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("100"), T("49600")],
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
                    query: "/* client A */ SELECT count(*), sum(id) FROM d WHERE b = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("51"), T("42301")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_stat_progress_cluster shows a CLUSTER rebuilding its indexes",
            set_up_script: &[
                "CREATE TABLE pr (id INT PRIMARY KEY, b INT);",
                "INSERT INTO pr SELECT g, g FROM generate_series(1, 100) g;",
                "CREATE FUNCTION pr_wait(x INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN PERFORM pg_advisory_xact_lock_shared(1); RETURN x; END $$;",
                "CREATE INDEX pr_f ON pr (pr_wait(b));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_lock(1);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CLUSTER pr USING pr_pkey;",
                    expected: Expected::Tag("CLUSTER"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT command, phase, cluster_index_relid::regclass::text, heap_tuples_scanned, heap_tuples_written, heap_blks_total, heap_blks_scanned, index_rebuild_count FROM pg_stat_progress_cluster WHERE relid = 'pr'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("command", TEXT), Column("phase", TEXT), Column("cluster_index_relid", TEXT), Column("heap_tuples_scanned", INT8), Column("heap_tuples_written", INT8), Column("heap_blks_total", INT8), Column("heap_blks_scanned", INT8), Column("index_rebuild_count", INT8)],
                        rows: &[
                            &[T("CLUSTER"), T("rebuilding index"), T("-"), T("100"), T("100"), T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_unlock(1);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indexrelid::regclass::text FROM pg_index WHERE indrelid = 'pr'::regclass AND indisclustered;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", TEXT)],
                        rows: &[
                            &[T("pr_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT count(*) FROM pg_stat_progress_cluster;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CLUSTER of a system catalog and the row order CLUSTER leaves",
            set_up_script: &[
                "CREATE TABLE ce (a INT, b TEXT);",
                "INSERT INTO ce SELECT 21 - g, 'p' || (21 - g) FROM generate_series(1, 20) g;",
                "CREATE INDEX ce_a ON ce (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CLUSTER pg_toast.pg_toast_826 USING pg_toast_826_index;",
                    expected: Expected::Tag("CLUSTER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM FULL pg_default_acl;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLUSTER ce USING ce_a;",
                    expected: Expected::Tag("CLUSTER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH r AS (SELECT ctid, lag(a) OVER (ORDER BY ctid) AS la, a FROM ce) SELECT count(*) FROM r WHERE la > a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("CLUSTER moves no rows, so their ctids keep the order they were written in rather than the index's (out/CLUSTER_PLAN.md, STORAGE_DESIGN Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisclustered FROM pg_index WHERE indexrelid = 'pg_toast.pg_toast_826_index'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisclustered", BOOL)],
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
            name: "CLUSTER of every clustered table skips one whose index went away meanwhile",
            set_up_script: &[
                "CREATE TABLE r1 (a INT PRIMARY KEY, b INT);",
                "CREATE TABLE r2 (a INT PRIMARY KEY, b INT);",
                "CREATE INDEX r2_b ON r2 (b);",
                "ALTER TABLE r1 CLUSTER ON r1_pkey;",
                "ALTER TABLE r2 CLUSTER ON r2_b;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ LOCK TABLE r2 IN ACCESS EXCLUSIVE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CLUSTER;",
                    expected: Expected::Tag("CLUSTER"),
                    flow: Flow::Exec,
                    expected_blocking: true,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP INDEX r2_b;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT indexrelid::regclass::text FROM pg_index WHERE indisclustered;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", TEXT)],
                        rows: &[
                            &[T("r1_pkey")],
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
fn test_vacuum_full_leaves_an_invalid_unique_index_invalid() {
    run_scripts(&[
        ScriptTest {
            name: "VACUUM FULL rebuilds indexes without checking constraints, leaving a failed unique index invalid",
            set_up_script: &[
                "CREATE TABLE vfh (f1 TEXT, f2 TEXT);",
                "INSERT INTO vfh VALUES ('a', 'b'), ('b', 'b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY vfi ON vfh (f2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"could not create unique index "vfi""#, detail: "Key (f2)=(b) is duplicated.", schema: "public", table: "vfh", constraint: "vfi", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM FULL vfh;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisvalid, indisready FROM pg_index WHERE indexrelid = 'vfi'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisvalid", BOOL), Column("indisready", BOOL)],
                        rows: &[
                            &[T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE vfh;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"could not create unique index "vfi""#, detail: "Key (f2)=(b) is duplicated.", schema: "public", table: "vfh", constraint: "vfi", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM vfh WHERE f1 = 'b';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE vfh;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisvalid, indisready FROM pg_index WHERE indexrelid = 'vfi'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisvalid", BOOL), Column("indisready", BOOL)],
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
    ]);
}

#[test]
fn test_a_cluster_waiting_in_an_index_expression_leaves_other_sessions_reading_the_storage_before_it() {
    run_scripts(&[
        ScriptTest {
            name: "other sessions read a table's committed storage while a CLUSTER of it waits in an index expression's function",
            set_up_script: &[
                "CREATE TABLE pv (id INT PRIMARY KEY, b INT);",
                "INSERT INTO pv SELECT g, g FROM generate_series(1, 100) g;",
                "CREATE FUNCTION pv_wait(x INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN PERFORM pg_advisory_xact_lock_shared(2); RETURN x; END $$;",
                "CREATE INDEX pv_f ON pv (pv_wait(b));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_lock(2);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CLUSTER pv USING pv_pkey;",
                    expected: Expected::Tag("CLUSTER"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT relname, relfilenode = oid FROM pg_class WHERE relname IN ('pv', 'pv_pkey', 'pv_f') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pv"), T("t")],
                            &[T("pv_f"), T("t")],
                            &[T("pv_pkey"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_unlock(2);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pv;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT count(*), sum(b) FROM pv;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("100"), T("5050")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
