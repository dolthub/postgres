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
fn test_tablespace() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE, ALTER, and DROP TABLESPACE",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE ROLE regress_ts_role;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLESPACE regress_ts LOCATION '';",
                    expected: Expected::Tag("CREATE TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLESPACE regress_rel LOCATION 'relative';",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "tablespace location must be an absolute path", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLESPACE pg_bad LOCATION '';",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "unacceptable tablespace name \"pg_bad\"", detail: "The prefix \"pg_\" is reserved for system tablespaces.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLESPACE regress_ts LOCATION '';",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "tablespace \"regress_ts\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLESPACE regress_q LOCATION '/tmp/it''s';",
                    expected: Expected::Error(Diagnostic { code: "42602", message: "tablespace location cannot contain single quotes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLESPACE regress_opt LOCATION '' WITH (bogus = 1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized parameter \"bogus\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLESPACE regress_tx LOCATION '';",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CREATE TABLESPACE cannot run inside a transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts SET (random_page_cost = 1.5, seq_page_cost = 1);",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts SET (bogus = 1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized parameter \"bogus\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts RESET (seq_page_cost);",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts RENAME TO pg_ts;",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "unacceptable tablespace name \"pg_ts\"", detail: "The prefix \"pg_\" is reserved for system tablespaces.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts RENAME TO pg_default;",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "unacceptable tablespace name \"pg_default\"", detail: "The prefix \"pg_\" is reserved for system tablespaces.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts RENAME TO regress_ts2;",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts2 OWNER TO regress_ts_role;",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON TABLESPACE regress_ts2 TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLESPACE regress_ts2 IS 'fast disk';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT spcname, spcowner::regrole AS owner, spcoptions, spcacl, shobj_description(oid, 'pg_tablespace') AS description FROM pg_tablespace ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("spcname", NAME), Column("owner", REGROLE), Column("spcoptions", TEXT_ARRAY), Column("spcacl", ACLITEM_ARRAY), Column("description", TEXT)],
                        rows: &[
                            &[T("pg_default"), T("postgres"), Null, Null, Null],
                            &[T("pg_global"), T("postgres"), Null, Null, Null],
                            &[T("regress_ts2"), T("regress_ts_role"), T("{random_page_cost=1.5}"), T("{regress_ts_role=C/regress_ts_role,=C/regress_ts_role}"), T("fast disk")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON TABLESPACE pg_default TO regress_ts_role;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE pg_default SET (random_page_cost = 3);",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLESPACE pg_default IS 'default';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE pg_global OWNER TO regress_ts_role;",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CREATE ON TABLESPACE regress_ts2 FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT spcname, spcowner::regrole AS owner, spcoptions, spcacl, shobj_description(oid, 'pg_tablespace') AS description FROM pg_tablespace ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("spcname", NAME), Column("owner", REGROLE), Column("spcoptions", TEXT_ARRAY), Column("spcacl", ACLITEM_ARRAY), Column("description", TEXT)],
                        rows: &[
                            &[T("pg_default"), T("postgres"), T("{random_page_cost=3}"), T("{postgres=C/postgres,regress_ts_role=C/postgres}"), T("default")],
                            &[T("pg_global"), T("regress_ts_role"), Null, Null, Null],
                            &[T("regress_ts2"), T("regress_ts_role"), T("{random_page_cost=1.5}"), T("{regress_ts_role=C/regress_ts_role}"), T("fast disk")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE pg_default;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for tablespace pg_default", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_none;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "tablespace \"regress_none\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE IF EXISTS regress_none;",
                    expected: Expected::Tag("DROP TABLESPACE"),
                    notices: &[Diagnostic { code: "00000", message: "tablespace \"regress_none\" does not exist, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts2;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "DROP TABLESPACE cannot run inside a transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts2;",
                    expected: Expected::Tag("DROP TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT spcname FROM pg_tablespace ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("spcname", NAME)],
                        rows: &[
                            &[T("pg_default")],
                            &[T("pg_global")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_shdescription WHERE classoid = 'pg_tablespace'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "Relations in a tablespace",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_ts LOCATION '';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (a int PRIMARY KEY, b text) TABLESPACE regress_ts;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 SELECT g, 'x' || g FROM generate_series(1, 200) g;",
                    expected: Expected::Tag("INSERT 0 200"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t1_b ON t1 (b) TABLESPACE regress_ts;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t1_b2 ON t1 (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_tablespace = regress_ts;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t2_a ON t2 (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET default_tablespace;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p (a int) PARTITION BY RANGE (a) TABLESPACE regress_ts;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (1) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (10) TO (20) TABLESPACE pg_default;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('t1', 't1_b', 't1_b2', 't1_pkey', 't2', 't2_a', 'p', 'p1', 'p2') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("p"), T("regress_ts")],
                            &[T("p1"), T("regress_ts")],
                            &[T("p2"), Null],
                            &[T("t1"), T("regress_ts")],
                            &[T("t1_b"), T("regress_ts")],
                            &[T("t1_b2"), Null],
                            &[T("t1_pkey"), Null],
                            &[T("t2"), T("regress_ts")],
                            &[T("t2_a"), T("regress_ts")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tablename, tablespace FROM pg_tables WHERE tablename IN ('t1', 't2', 'p', 'p1', 'p2') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME), Column("tablespace", NAME)],
                        rows: &[
                            &[T("p"), T("regress_ts")],
                            &[T("p1"), T("regress_ts")],
                            &[T("p2"), Null],
                            &[T("t1"), T("regress_ts")],
                            &[T("t2"), T("regress_ts")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, tablespace FROM pg_indexes WHERE tablename IN ('t1', 't2') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("tablespace", NAME)],
                        rows: &[
                            &[T("t1_b"), T("regress_ts")],
                            &[T("t1_b2"), Null],
                            &[T("t1_pkey"), Null],
                            &[T("t2_a"), T("regress_ts")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('t1_b'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T("CREATE INDEX t1_b ON public.t1 USING btree (b)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM t1 WHERE b LIKE 'x%';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("200"), T("20100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (a int) TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_tablespace = regress_none;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid value for parameter \"default_tablespace\": \"regress_none\"", detail: "Tablespace \"regress_none\" does not exist.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_ts_user;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO regress_ts_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_ts_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (a int) TABLESPACE regress_ts;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for tablespace regress_ts", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE fn AS SELECT pg_relation_filenode('t1') AS f;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT f FROM fn) <> pg_relation_filenode('t1') AS new_filenode;",
                    expected: Expected::Rows {
                        columns: &[Column("new_filenode", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX t1_pkey SET TABLESPACE regress_ts;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 SET TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('t1', 't1_pkey', 'p', 'p1') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("p"), Null],
                            &[T("p1"), T("regress_ts")],
                            &[T("t1"), Null],
                            &[T("t1_pkey"), T("regress_ts")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t1 WHERE b = 'x150';",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("150")],
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
                    query: "ALTER TABLE ALL IN TABLESPACE regress_ts SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX ALL IN TABLESPACE regress_ts OWNED BY postgres SET TABLESPACE pg_default NOWAIT;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER MATERIALIZED VIEW ALL IN TABLESPACE regress_ts SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER MATERIALIZED VIEW"),
                    notices: &[Diagnostic { code: "P0002", message: "no matching relations in tablespace \"regress_ts\" found", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('t1', 't1_b', 't1_pkey', 't2', 't2_a', 'p', 'p1') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("p"), Null],
                            &[T("p1"), Null],
                            &[T("t1"), Null],
                            &[T("t1_b"), Null],
                            &[T("t1_pkey"), Null],
                            &[T("t2"), Null],
                            &[T("t2_a"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("200"), T("20100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 SET TABLESPACE regress_ts;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "tablespace \"regress_ts\" is not empty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p3 (a int) PARTITION BY LIST (a) TABLESPACE regress_ts;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass AS catalog, refclassid::regclass AS refcatalog, deptype FROM pg_shdepend WHERE refclassid = 'pg_tablespace'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("catalog", REGCLASS), Column("refcatalog", REGCLASS), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_class"), T("pg_tablespace"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "tablespace \"regress_ts\" cannot be dropped because some objects depend on it", detail: "tablespace for table p3", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE p3;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts;",
                    expected: Expected::Tag("DROP TABLESPACE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REINDEX with a tablespace",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_ts LOCATION '';",
                "CREATE TABLE r (a int PRIMARY KEY, b int);",
                "INSERT INTO r SELECT g, g FROM generate_series(1, 100) g;",
                "CREATE INDEX r_b ON r (b);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "REINDEX (TABLESPACE regress_ts) INDEX r_b;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('r', 'r_b', 'r_pkey') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("r"), Null],
                            &[T("r_b"), T("regress_ts")],
                            &[T("r_pkey"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX (TABLESPACE regress_ts) TABLE r;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('r', 'r_b', 'r_pkey') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("r"), Null],
                            &[T("r_b"), T("regress_ts")],
                            &[T("r_pkey"), T("regress_ts")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX (TABLESPACE pg_default) TABLE r;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX (TABLESPACE regress_ts, CONCURRENTLY) TABLE r;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('r', 'r_b', 'r_pkey') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("r"), Null],
                            &[T("r_b"), T("regress_ts")],
                            &[T("r_pkey"), T("regress_ts")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX (TABLESPACE pg_default, CONCURRENTLY) INDEX r_b;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('r', 'r_b', 'r_pkey') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("r"), Null],
                            &[T("r_b"), Null],
                            &[T("r_pkey"), T("regress_ts")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM r WHERE b = 51;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("51")],
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
                    query: "REINDEX (TABLESPACE pg_global) TABLE r;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX (TABLESPACE regress_none) TABLE r;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "tablespace \"regress_none\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A tablespace's page costs",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_ts LOCATION '' WITH (seq_page_cost = 4, random_page_cost = 8);",
                "CREATE TABLE c0 (a int);",
                "CREATE TABLE c1 (a int) TABLESPACE regress_ts;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM c0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on c0  (cost=0.00..35.50 rows=2550 width=4)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM c1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on c1  (cost=0.00..65.50 rows=2550 width=4)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts SET (seq_page_cost = 0.5);",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM c1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on c1  (cost=0.00..30.50 rows=2550 width=4)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE pg_default SET (seq_page_cost = 2);",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM c0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on c0  (cost=0.00..45.50 rows=2550 width=4)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Roles that own or hold privileges on tablespaces",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE ROLE regress_tsr1;",
                "CREATE ROLE regress_tsr2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLESPACE regress_ts OWNER regress_tsr1 LOCATION '';",
                    expected: Expected::Tag("CREATE TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON TABLESPACE regress_ts TO regress_tsr2;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON TABLESPACE pg_default TO regress_tsr2;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass AS catalog, refobjid::regrole AS role, deptype FROM pg_shdepend WHERE refobjid IN ('regress_tsr1'::regrole, 'regress_tsr2'::regrole) ORDER BY 2, 1, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("catalog", REGCLASS), Column("role", REGROLE), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_tablespace"), T("regress_tsr1"), T("o")],
                            &[T("pg_tablespace"), T("regress_tsr2"), T("a")],
                            &[T("pg_tablespace"), T("regress_tsr2"), T("a")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_tsr1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "role \"regress_tsr1\" cannot be dropped because some objects depend on it", detail: "owner of tablespace regress_ts", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_tsr2;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "role \"regress_tsr2\" cannot be dropped because some objects depend on it", detail: "privileges for tablespace pg_default
privileges for tablespace regress_ts", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY regress_tsr1 TO regress_tsr2;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT spcname, spcowner::regrole AS owner, spcacl FROM pg_tablespace ORDER BY spcname;",
                    expected: Expected::Rows {
                        columns: &[Column("spcname", NAME), Column("owner", REGROLE), Column("spcacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("pg_default"), T("postgres"), T("{postgres=C/postgres,regress_tsr2=C/postgres}")],
                            &[T("pg_global"), T("postgres"), Null],
                            &[T("regress_ts"), T("regress_tsr2"), T("{regress_tsr2=C/regress_tsr2}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_tsr2;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT spcname, spcowner::regrole AS owner, spcacl FROM pg_tablespace ORDER BY spcname;",
                    expected: Expected::Rows {
                        columns: &[Column("spcname", NAME), Column("owner", REGROLE), Column("spcacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("pg_default"), T("postgres"), T("{postgres=C/postgres}")],
                            &[T("pg_global"), T("postgres"), Null],
                            &[T("regress_ts"), T("regress_tsr2"), T("{regress_tsr2=C/regress_tsr2}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_tsr1;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLESPACE regress_ts OWNER TO postgres;",
                    expected: Expected::Tag("ALTER TABLESPACE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_tsr2;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts;",
                    expected: Expected::Tag("DROP TABLESPACE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Indexes keep their tablespaces when a partition or a rebuild makes them",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_ts LOCATION '';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE part (a int) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE part1 PARTITION OF part FOR VALUES IN (1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX part_a_idx ON part (a) TABLESPACE regress_ts;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE part2 PARTITION OF part FOR VALUES IN (2);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('part_a_idx', 'part1_a_idx', 'part2_a_idx') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("part1_a_idx"), T("regress_ts")],
                            &[T("part2_a_idx"), T("regress_ts")],
                            &[T("part_a_idx"), T("regress_ts")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tab (id bigint) TABLESPACE regress_ts;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX tab_i1 ON tab (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX tab_i2 ON tab (id) TABLESPACE regress_ts;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tab ADD CONSTRAINT tab_i3 PRIMARY KEY (id);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tab ADD CONSTRAINT tab_i4 UNIQUE (id) USING INDEX TABLESPACE regress_ts;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_tablespace TO regress_ts;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tab ALTER id TYPE int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('tab_i1', 'tab_i2', 'tab_i3', 'tab_i4') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("tab_i1"), Null],
                            &[T("tab_i2"), T("regress_ts")],
                            &[T("tab_i3"), Null],
                            &[T("tab_i4"), T("regress_ts")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_tablespace TO '';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tab ALTER id TYPE bigint;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.spcname FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname IN ('tab_i1', 'tab_i2', 'tab_i3', 'tab_i4') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("tab_i1"), Null],
                            &[T("tab_i2"), T("regress_ts")],
                            &[T("tab_i3"), Null],
                            &[T("tab_i4"), T("regress_ts")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Databases in tablespaces",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_ts LOCATION '';",
                "CREATE ROLE regress_l4_user CREATEDB;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE regress_l4 TABLESPACE regress_ts;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname, t.spcname FROM pg_database d JOIN pg_tablespace t ON t.oid = d.dattablespace WHERE d.datname = 'regress_l4';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("regress_l4"), T("regress_ts")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname FROM pg_tablespace_databases((SELECT oid FROM pg_tablespace WHERE spcname = 'regress_ts')) AS x(oid) JOIN pg_database d ON d.oid = x.oid ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("regress_l4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_tablespace_databases(1664);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_tablespace_databases", OID)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "global tablespace never has databases", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_tablespace_databases(12345);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_tablespace_databases", OID)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "12345 is not a tablespace OID", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE regress_l4_global TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "pg_global cannot be used as default tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE regress_l4_none TABLESPACE regress_none;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "tablespace \"regress_none\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_l4_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE regress_l4_denied TABLESPACE regress_ts;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for tablespace regress_ts", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_l4 SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname, t.spcname FROM pg_database d JOIN pg_tablespace t ON t.oid = d.dattablespace WHERE d.datname = 'regress_l4';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("regress_l4"), T("pg_default")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname FROM pg_tablespace_databases((SELECT oid FROM pg_tablespace WHERE spcname = 'regress_ts')) AS x(oid) JOIN pg_database d ON d.oid = x.oid ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_l4 WITH TABLESPACE regress_ts;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname, t.spcname FROM pg_database d JOIN pg_tablespace t ON t.oid = d.dattablespace WHERE d.datname = 'regress_l4';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("spcname", NAME)],
                        rows: &[
                            &[T("regress_l4"), T("regress_ts")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_l4 SET TABLESPACE regress_ts;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_l4 SET TABLESPACE pg_default;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "ALTER DATABASE SET TABLESPACE cannot run inside a transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_l4 SET TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "pg_global cannot be used as default tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_l4 SET TABLESPACE regress_none;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "tablespace \"regress_none\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_none SET TABLESPACE pg_default;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"regress_none\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_l4 WITH CONNECTION LIMIT 5 TABLESPACE pg_default;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "option \"tablespace\" cannot be specified with other options", position: 51, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_l4_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE regress_l4 SET TABLESPACE pg_default;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of database regress_l4", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "tablespace \"regress_ts\" is not empty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE regress_l4;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts;",
                    expected: Expected::Tag("DROP TABLESPACE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Sizes of databases and tablespaces need privileges",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_ts LOCATION '';",
                "CREATE DATABASE regress_locked;",
                "REVOKE CONNECT ON DATABASE regress_locked FROM PUBLIC;",
                "CREATE ROLE regress_plain;",
                "CREATE ROLE regress_stats IN ROLE pg_read_all_stats;",
                "GRANT CREATE ON TABLESPACE regress_ts TO regress_plain;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET ROLE regress_plain;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_database_size('regress_locked') > 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for database regress_locked", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_database_size('postgres') > 0;",
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
                    query: "SELECT pg_tablespace_size('regress_ts') >= 0;",
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
                    query: "SELECT pg_tablespace_size('pg_default') > 0;",
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
                    query: "SELECT pg_tablespace_size('pg_global') > 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for tablespace pg_global", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_tablespace_size(1664) > 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for tablespace pg_global", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_stats;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_database_size('regress_locked') > 0;",
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
                    query: "SELECT pg_tablespace_size('pg_global') > 0;",
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
                    query: "SELECT pg_tablespace_size('regress_ts') >= 0;",
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
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Temporary tablespaces",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_ts1 LOCATION '';",
                "CREATE TABLESPACE regress_ts2 LOCATION '';",
                "CREATE ROLE regress_plain;",
                "GRANT CREATE ON SCHEMA public TO regress_plain;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = 'regress_none';",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "tablespace \"regress_none\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = 'regress_ts1,\"bad';",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "tablespace \"regress_ts1,\"bad\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = regress_ts1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW temp_tablespaces;",
                    expected: Expected::Rows {
                        columns: &[Column("temp_tablespaces", TEXT)],
                        rows: &[
                            &[T("regress_ts1")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE t1 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = '';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE t2 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = pg_default;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE t3 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, COALESCE(t.spcname, '(own)') AS spc FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relpersistence = 't' AND c.relkind = 'r' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spc", NAME)],
                        rows: &[
                            &[T("t1"), T("regress_ts1")],
                            &[T("t2"), T("(own)")],
                            &[T("t3"), T("(own)")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_plain;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = regress_ts1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for tablespace regress_ts1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE regress_plain SET temp_tablespaces = 'regress_none';",
                    expected: Expected::Tag("ALTER ROLE"),
                    notices: &[Diagnostic { code: "42704", message: "tablespace \"regress_none\" does not exist", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE regress_plain SET temp_tablespaces = 'regress_ts1';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE regress_plain RESET temp_tablespaces;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f() RETURNS TEXT LANGUAGE plpgsql SET temp_tablespaces = regress_ts2 AS $$ BEGIN CREATE TEMP TABLE t4 (x INT); RETURN (SELECT t.spcname FROM pg_class c JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname = 't4'); END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = regress_ts1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f();",
                    expected: Expected::Rows {
                        columns: &[Column("f", TEXT)],
                        rows: &[
                            &[T("regress_ts2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = regress_ts2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_plain;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
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
                ScriptTestAssertion {
                    query: "GRANT CREATE ON TABLESPACE regress_ts2 TO regress_plain;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_plain;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE t5 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, spc FROM (SELECT c.relname, COALESCE(t.spcname, '(own)') AS spc FROM pg_class c LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relpersistence = 't' AND c.relkind = 'r' ORDER BY 1) p WHERE relname IN ('t4', 't5');",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("spc", NAME)],
                        rows: &[
                            &[T("t4"), T("regress_ts2")],
                            &[T("t5"), T("regress_ts2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts1;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "tablespace \"regress_ts1\" is not empty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD TEMP;",
                    expected: Expected::Tag("DISCARD TEMP"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_ts1;",
                    expected: Expected::Tag("DROP TABLESPACE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Temporary files in temporary tablespaces",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_ts1 LOCATION '';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET temp_tablespaces = regress_ts1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT g FROM generate_series(1, 20000) g ORDER BY g DESC;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4)],
                        rows: &[
                            &[T("20000")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) > 0 AS listed FROM pg_ls_tmpdir((SELECT oid FROM pg_tablespace WHERE spcname = 'regress_ts1'));",
                    expected: Expected::Rows {
                        columns: &[Column("listed", BOOL)],
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
                    query: "SELECT count(*) > 0 AS listed FROM pg_ls_tmpdir((SELECT oid FROM pg_tablespace WHERE spcname = 'regress_ts1'));",
                    expected: Expected::Rows {
                        columns: &[Column("listed", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_ls_tmpdir(1664);",
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
    ]);
}

#[test]
fn test_filenode_relation_by_tablespace() {
    run_scripts(&[
        ScriptTest {
            name: "pg_filenode_relation leaves out temporary relations and matches the tablespace",
            set_up_script: &[
                "SET allow_in_place_tablespaces = true;",
                "CREATE TABLESPACE regress_fn LOCATION '';",
                "CREATE TABLE in_space (a INT) TABLESPACE regress_fn;",
                "CREATE TABLE at_home (a INT);",
                "CREATE TEMP TABLE relation_filenode_check (c1 INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relpersistence, pg_filenode_relation (reltablespace, pg_relation_filenode(oid)) FROM pg_class WHERE relname = 'relation_filenode_check';",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR), Column("pg_filenode_relation", REGCLASS)],
                        rows: &[
                            &[T("t"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_filenode_relation((SELECT oid FROM pg_tablespace WHERE spcname = 'regress_fn'), pg_relation_filenode('in_space'))::regclass AS by_space, pg_filenode_relation(0, pg_relation_filenode('in_space')) AS by_default;",
                    expected: Expected::Rows {
                        columns: &[Column("by_space", REGCLASS), Column("by_default", REGCLASS)],
                        rows: &[
                            &[T("in_space"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_filenode_relation(0, pg_relation_filenode('at_home'))::regclass AS by_zero, pg_filenode_relation((SELECT dattablespace FROM pg_database WHERE datname = current_database()), pg_relation_filenode('at_home'))::regclass AS by_own, pg_filenode_relation((SELECT oid FROM pg_tablespace WHERE spcname = 'regress_fn'), pg_relation_filenode('at_home')) AS by_other;",
                    expected: Expected::Rows {
                        columns: &[Column("by_zero", REGCLASS), Column("by_own", REGCLASS), Column("by_other", REGCLASS)],
                        rows: &[
                            &[T("at_home"), T("at_home"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE in_space;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLESPACE regress_fn;",
                    expected: Expected::Tag("DROP TABLESPACE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
