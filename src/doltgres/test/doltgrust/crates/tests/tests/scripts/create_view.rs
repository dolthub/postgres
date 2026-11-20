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
fn test_create_view_statements() {
    run_scripts(&[
        ScriptTest {
            name: "basic create view statements",
            set_up_script: &[
                "create table t1 (pk int);",
                "insert into t1 values (1), (2), (3), (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "create view v as select * from t1 order by pk;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from v order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "views on different schemas",
            set_up_script: &[
                "CREATE SCHEMA testschema;",
                "SET search_path TO testschema;",
                "CREATE TABLE testing (pk INT primary key, v2 TEXT);",
                "INSERT INTO testing VALUES (1,'a'), (2,'b'), (3,'c');",
                "CREATE VIEW testview AS SELECT * FROM testing;",
                "CREATE SCHEMA myschema;",
                "SET search_path TO myschema;",
                "CREATE TABLE mytable (pk INT primary key, v1 INT);",
                "INSERT INTO mytable VALUES (1,4), (2,5), (3,6);",
                "CREATE VIEW myview AS SELECT * FROM mytable;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SHOW search_path;",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T("myschema")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select v1 from myview order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select v2 from testview order by pk;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "testview" does not exist"#, position: 16, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select v2 from testschema.testview order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select name from dolt_schemas;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("myview")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = 'testschema';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "SHOW search_path;",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T("testschema")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "select * from myview order by pk; /* err */",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "myview" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select v1 from myschema.myview order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select v2 from testview order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select name from dolt_schemas;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("testview")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = testschema, myschema;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "SHOW search_path;",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T("testschema, myschema")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select v1 from myview order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select v2 from testview order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select name from dolt_schemas;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("testview")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select name from myschema.dolt_schemas;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("myview")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "view lookup follows relation order on search_path",
            set_up_script: &[
                "CREATE SCHEMA first_schema",
                "CREATE SCHEMA second_schema",
                "CREATE TABLE second_schema.source (v INT)",
                "INSERT INTO second_schema.source VALUES (42)",
                "CREATE VIEW second_schema.later_view AS SELECT v FROM second_schema.source",
                "CREATE TABLE first_schema.shadow (v INT)",
                "INSERT INTO first_schema.shadow VALUES (10)",
                "CREATE VIEW second_schema.shadow AS SELECT 20 AS v",
                "CREATE VIEW first_schema.first_view AS SELECT 30 AS v",
                "CREATE TABLE second_schema.first_view (v INT)",
                "INSERT INTO second_schema.first_view VALUES (40)",
                "CREATE VIEW first_schema.same_view AS SELECT 1 AS v",
                "CREATE VIEW second_schema.same_view AS SELECT 2 AS v",
                "SET search_path TO first_schema, second_schema",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM later_view",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM second_schema.later_view",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM shadow",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM first_view",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM same_view",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path TO missing_schema, second_schema, first_schema",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM later_view",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM shadow",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM first_view",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("40")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM same_view",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
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
        ScriptTest {
            name: "view in later search_path schema reaches privilege checks",
            set_up_script: &[
                "CREATE SCHEMA empty_schema",
                "CREATE SCHEMA protected_schema",
                "CREATE VIEW protected_schema.target_view AS SELECT 42 AS v",
                "CREATE ROLE allowed_reader LOGIN PASSWORD 'password'",
                "CREATE ROLE denied_reader LOGIN PASSWORD 'password'",
                "GRANT USAGE ON SCHEMA empty_schema, protected_schema TO allowed_reader, denied_reader",
                "GRANT SELECT ON protected_schema.target_view TO allowed_reader",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET search_path TO empty_schema, protected_schema",
                    expected: Expected::Tag("SET"),
                    username: "allowed_reader",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM target_view",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "allowed_reader",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path TO empty_schema, protected_schema",
                    expected: Expected::Tag("SET"),
                    username: "denied_reader",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM target_view",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for view target_view", ..E }),
                    username: "denied_reader",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create view from view",
            set_up_script: &[
                "create table t1 (pk int);",
                "insert into t1 values (1), (2), (3), (1);",
                "create view v as select * from t1 where pk > 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "create view v1 as select * from v order by pk;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from v1 order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "view with expression name",
            set_up_script: &[
                "create view v as select 2+2",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * from v;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "view with column names",
            set_up_script: &[
                "CREATE TABLE xy (x int primary key, y int);",
                "insert into xy values (1, 4), (4, 9)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "create view v_today(today) as select 2",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW xyv (u,v) AS SELECT * from xy",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v from xyv;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("9")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT today from v_today;",
                    expected: Expected::Rows {
                        columns: &[Column("today", INT4)],
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
        ScriptTest {
            name: "nested view",
            set_up_script: &[
                "create table t1 (pk int);",
                "insert into t1 values (1), (2), (3), (4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "create view unionView as (select * from t1 order by pk desc limit 1) union all (select * from t1 order by pk limit 1)",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from unionView order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cast (postgres-specific syntax)",
            set_up_script: &[
                "create table t1 (pk int);",
                "insert into t1 values (1), (2), (3), (4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT pk::INT2 FROM t1 ORDER BY pk;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from v order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT2)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_text AS SELECT pk::int2, (pk)::text AS pk_text FROM t1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pk_text from v_text order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk_text", TEXT)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "not yet supported create view queries",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY VIEW v AS SELECT 1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE RECURSIVE VIEW v AS SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "AS""#, position: 25, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT 1 WITH LOCAL CHECK OPTION;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "WITH CHECK OPTION is supported only on automatically updatable views", hint: "Views that do not select from a single table or view are not automatically updatable.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v WITH (check_option = 'local') AS SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "WITH CHECK OPTION is supported only on automatically updatable views", hint: "Views that do not select from a single table or view are not automatically updatable.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v WITH (security_barrier = true) AS SELECT 1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create view with CTE",
            set_up_script: &[
                "CREATE TABLE public.t1 (id integer NOT NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "create view public.v1 as with table1 as (select * from t1) select id from table1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create view with custom type in its select statement",
            set_up_script: &[
                "CREATE TYPE e AS ENUM ('sched', 'busy', 'final', 'help');",
                "CREATE TABLE t (id integer NOT NULL, t e);",
                "INSERT INTO t VALUES (1, 'busy'), (2, 'final'), (3, 'busy'), (4, 'help');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "create view v as select * from t where (t = 'busy'::e);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("busy")],
                            &[T("3"), T("busy")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "View names must be unique across all relation types",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int PRIMARY KEY, v1 int);",
                "CREATE SEQUENCE seq1;",
                "CREATE VIEW existing_view AS SELECT pk FROM tbl1;",
                "CREATE INDEX idx1 ON tbl1 (v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW tbl1 AS SELECT pk FROM tbl1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "tbl1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW tbl1 AS SELECT pk FROM tbl1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""tbl1" is not a view"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW seq1 AS SELECT pk FROM tbl1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "seq1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW seq1 AS SELECT pk FROM tbl1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""seq1" is not a view"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW existing_view AS SELECT pk FROM tbl1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "existing_view" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW existing_view AS SELECT pk FROM tbl1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW idx1 AS SELECT pk FROM tbl1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "idx1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW idx1 AS SELECT pk FROM tbl1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""idx1" is not a view"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Views that reference each other",
            set_up_script: &[
                "CREATE TABLE cyc_t1 (a BIGINT);",
                "CREATE TABLE cyc_t2 (b BIGINT);",
                "CREATE VIEW cyc_v2 AS SELECT * FROM cyc_t1, cyc_t2;",
                "CREATE VIEW cyc_v3 AS SELECT * FROM cyc_v2;",
                "CREATE OR REPLACE VIEW cyc_v2 AS SELECT * FROM cyc_v3;",
                "CREATE VIEW cyc_self AS SELECT 1 AS x;",
                "CREATE OR REPLACE VIEW cyc_self AS SELECT * FROM cyc_self;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM cyc_v2;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in rules for relation "cyc_v2""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cyc_v3;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in rules for relation "cyc_v3""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT * FROM cyc_v3) sq;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in rules for relation "cyc_v3""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cyc_self;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in rules for relation "cyc_self""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cyc_v2;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in rules for relation "cyc_v2""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, a.attname FROM pg_catalog.pg_class c JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid WHERE c.relname IN ('cyc_t1', 'cyc_t2') AND a.attnum > 0 ORDER BY c.relname, a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("attname", NAME)],
                        rows: &[
                            &[T("cyc_t1"), T("a")],
                            &[T("cyc_t2"), T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, a.attname FROM pg_catalog.pg_class c JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid WHERE c.relname IN ('cyc_v2', 'cyc_v3') ORDER BY c.relname, a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("attname", NAME)],
                        rows: &[
                            &[T("cyc_v2"), T("a")],
                            &[T("cyc_v2"), T("b")],
                            &[T("cyc_v3"), T("a")],
                            &[T("cyc_v3"), T("b")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW cyc_v4 AS SELECT * FROM cyc_v2;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Views with the same name in different schemas",
            set_up_script: &[
                "CREATE SCHEMA cyc_s1;",
                "CREATE SCHEMA cyc_s2;",
                "CREATE TABLE cyc_s2.t (a INT);",
                "INSERT INTO cyc_s2.t VALUES (1);",
                "CREATE VIEW cyc_s2.v AS SELECT a FROM cyc_s2.t;",
                "CREATE VIEW cyc_s1.v AS SELECT a FROM cyc_s2.v;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM cyc_s1.v;",
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
fn test_view_definitions() {
    run_scripts(&[
        ScriptTest {
            name: "pg_get_viewdef and pg_views definitions",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE a (id int PRIMARY KEY, name text, d date, n numeric);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b (id int, a_id int, x varchar(10));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v1 AS SELECT name FROM a;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v2 AS SELECT * FROM a WHERE id > 1 AND name LIKE 'x%' ORDER BY name DESC, id LIMIT 10 OFFSET 2;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v3 AS SELECT a.id, b.x AS bx, a.id + 1 AS next, upper(a.name), count(*) OVER () FROM a JOIN b ON a.id = b.a_id LEFT JOIN b b2 ON b2.id = b.id;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v4 AS SELECT a_id, count(*), sum(id) AS total FROM b GROUP BY a_id HAVING count(*) > 1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v5 AS SELECT id FROM a UNION SELECT id FROM b UNION ALL SELECT 3;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v6 AS SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id) AND id IN (SELECT a_id FROM b) AND name = (SELECT max(x) FROM b);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v7 (c1, c2) AS SELECT DISTINCT id, CASE WHEN id > 1 THEN 'big' ELSE 'small' END FROM a;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v8 AS WITH t AS (SELECT id FROM a) SELECT t.id FROM t, b WHERE t.id = b.id;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v9 AS SELECT s.x FROM (SELECT id AS x FROM a) s, generate_series(1, 3) g(n) WHERE s.x = g.n;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v10 AS SELECT 1 AS one, 'a'::text, now(), current_date, coalesce(name, 'z'), id::text, n::int FROM a;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v11 AS SELECT id, d FROM a WHERE d > '2020-01-01' AND id BETWEEN 1 AND 5 AND name IS NOT NULL AND id IN (1, 2, 3);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v12 AS SELECT * FROM (VALUES (1, 'one'), (2, 'two')) AS v(num, word);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v13 AS SELECT extract(year FROM d), substring(name FROM 2 FOR 3), trim(name), position('a' IN name) FROM a;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v14 AS SELECT b.id FROM a CROSS JOIN b;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v15 AS SELECT id FROM a INTERSECT SELECT id FROM b EXCEPT SELECT 5;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v16 AS SELECT a.id FROM a JOIN b USING (id);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v17 AS SELECT DISTINCT ON (a_id) a_id, x FROM b ORDER BY a_id, x DESC NULLS LAST;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT viewname, definition FROM pg_views WHERE schemaname = 'public' ORDER BY viewname;",
                    expected: Expected::Rows {
                        columns: &[Column("viewname", NAME), Column("definition", TEXT)],
                        rows: &[
                            &[T("v1"), T(r#" SELECT name
   FROM a;"#)],
                            &[T("v10"), T(r#" SELECT 1 AS one,
    'a'::text AS text,
    now() AS now,
    CURRENT_DATE AS "current_date",
    COALESCE(name, 'z'::text) AS "coalesce",
    (id)::text AS id,
    (n)::integer AS n
   FROM a;"#)],
                            &[T("v11"), T(r#" SELECT id,
    d
   FROM a
  WHERE ((d > '2020-01-01'::date) AND ((id >= 1) AND (id <= 5)) AND (name IS NOT NULL) AND (id = ANY (ARRAY[1, 2, 3])));"#)],
                            &[T("v12"), T(r#" SELECT num,
    word
   FROM ( VALUES (1,'one'::text), (2,'two'::text)) v(num, word);"#)],
                            &[T("v13"), T(r#" SELECT EXTRACT(year FROM d) AS "extract",
    SUBSTRING(name FROM 2 FOR 3) AS "substring",
    TRIM(BOTH FROM name) AS btrim,
    POSITION(('a'::text) IN (name)) AS "position"
   FROM a;"#)],
                            &[T("v14"), T(r#" SELECT b.id
   FROM (a
     CROSS JOIN b);"#)],
                            &[T("v15"), T(r#"(
         SELECT a.id
           FROM a
        INTERSECT
         SELECT b.id
           FROM b
) EXCEPT
 SELECT 5 AS id;"#)],
                            &[T("v16"), T(r#" SELECT a.id
   FROM (a
     JOIN b USING (id));"#)],
                            &[T("v17"), T(r#" SELECT DISTINCT ON (a_id) a_id,
    x
   FROM b
  ORDER BY a_id, x DESC NULLS LAST;"#)],
                            &[T("v2"), T(r#" SELECT id,
    name,
    d,
    n
   FROM a
  WHERE ((id > 1) AND (name ~~ 'x%'::text))
  ORDER BY name DESC, id
 OFFSET 2
 LIMIT 10;"#)],
                            &[T("v3"), T(r#" SELECT a.id,
    b.x AS bx,
    (a.id + 1) AS next,
    upper(a.name) AS upper,
    count(*) OVER () AS count
   FROM ((a
     JOIN b ON ((a.id = b.a_id)))
     LEFT JOIN b b2 ON ((b2.id = b.id)));"#)],
                            &[T("v4"), T(r#" SELECT a_id,
    count(*) AS count,
    sum(id) AS total
   FROM b
  GROUP BY a_id
 HAVING (count(*) > 1);"#)],
                            &[T("v5"), T(r#"(
         SELECT a.id
           FROM a
        UNION
         SELECT b.id
           FROM b
) UNION ALL
 SELECT 3 AS id;"#)],
                            &[T("v6"), T(r#" SELECT id
   FROM a
  WHERE ((EXISTS ( SELECT 1
           FROM b
          WHERE (b.a_id = a.id))) AND (id IN ( SELECT b.a_id
           FROM b)) AND (name = ( SELECT max((b.x)::text) AS max
           FROM b)));"#)],
                            &[T("v7"), T(r#" SELECT DISTINCT id AS c1,
        CASE
            WHEN (id > 1) THEN 'big'::text
            ELSE 'small'::text
        END AS c2
   FROM a;"#)],
                            &[T("v8"), T(r#" WITH t AS (
         SELECT a.id
           FROM a
        )
 SELECT t.id
   FROM t,
    b
  WHERE (t.id = b.id);"#)],
                            &[T("v9"), T(r#" SELECT s.x
   FROM ( SELECT a.id AS x
           FROM a) s,
    generate_series(1, 3) g(n)
  WHERE (s.x = g.n);"#)],
                        ],
                        tag: "SELECT 17",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('v3'::regclass, true), pg_get_viewdef('v6', true), pg_get_viewdef('v1'::regclass::oid, 10);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT), Column("pg_get_viewdef", TEXT), Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(r#" SELECT a.id,
    b.x AS bx,
    a.id + 1 AS next,
    upper(a.name) AS upper,
    count(*) OVER () AS count
   FROM a
     JOIN b ON a.id = b.a_id
     LEFT JOIN b b2 ON b2.id = b.id;"#), T(r#" SELECT id
   FROM a
  WHERE (EXISTS ( SELECT 1
           FROM b
          WHERE b.a_id = a.id)) AND (id IN ( SELECT b.a_id
           FROM b)) AND name = (( SELECT max(b.x::text) AS max
           FROM b));"#), T(r#" SELECT name
   FROM a;"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_get_viewdef with windows, aggregates, functions, and pretty-printing",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT string_agg(relname, ',') FROM (SELECT relname FROM pg_class WHERE relkind='v' AND relnamespace='information_schema'::regnamespace LIMIT 6) s; SELECT count(*) FROM information_schema.views; SELECT count(*) FROM pg_class WHERE relkind='v';",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "cannot insert multiple commands into a prepared statement", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('pg_catalog.pg_roles'::regclass) = definition FROM pg_views WHERE viewname = 'pg_roles';",
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
fn test_view_and_routine_rules() {
    run_scripts(&[
        ScriptTest {
            name: "WITH queries in INSERT, UPDATE, and DELETE",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE w1 (a INT PRIMARY KEY, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH src AS (SELECT 1 AS a, 'x' AS b UNION ALL SELECT 2, 'y') INSERT INTO w1 SELECT * FROM src;",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH v AS (SELECT 5 AS n) INSERT INTO w1 VALUES ((SELECT n FROM v), 'z') RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("5"), T("z")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH t AS (SELECT 2 AS a) UPDATE w1 SET b = 'updated' WHERE a IN (SELECT a FROM t) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("updated")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH t AS (SELECT 1 AS a) DELETE FROM w1 USING t WHERE w1.a = t.a RETURNING w1.*;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH t AS (SELECT 1 AS a), t AS (SELECT 2) INSERT INTO w1 VALUES (9, 'q');",
                    expected: Expected::Error(Diagnostic { code: "42712", message: r#"WITH query name "t" specified more than once"#, position: 28, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM w1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("updated")],
                            &[T("5"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Views over other relations and check options",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE vt (pk INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE vs;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW vt AS SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""vt" is not a view"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW vs AS SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""vs" is not a view"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW vc AS SELECT 1 WITH LOCAL CHECK OPTION;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "WITH CHECK OPTION is supported only on automatically updatable views", hint: "Views that do not select from a single table or view are not automatically updatable.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW vc WITH (check_option = 'cascaded') AS SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "WITH CHECK OPTION is supported only on automatically updatable views", hint: "Views that do not select from a single table or view are not automatically updatable.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW vc WITH (security_barrier = true) AS SELECT 1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW vd AS SELECT pk FROM vt WITH CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vc;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
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
            name: "Composite routine parameters and results",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE SCHEMA rsch;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE rsch.pair AS (id INT, label TEXT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE goods (id INT PRIMARY KEY, name TEXT NOT NULL, qty INT NOT NULL, price REAL NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO goods VALUES (1, 'apple', 3, 2.5), (2, 'banana', 5, 1.2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION total(g goods) RETURNS REAL AS $$ BEGIN RETURN g.qty * g.price; END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT total(g) FROM goods AS g ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("total", FLOAT4)],
                        rows: &[
                            &[T("6")],
                            &[T("7.5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pairs() RETURNS TABLE(p rsch.pair) LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT 1, 'one'; RETURN QUERY SELECT 2, 'two'; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pairs();",
                    expected: Expected::Rows {
                        columns: &[Column("pairs", USER_DEFINED)],
                        rows: &[
                            &[T("(1,one)")],
                            &[T("(2,two)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pairs();",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("label", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pairs_sql() RETURNS TABLE(p rsch.pair) LANGUAGE sql AS $$ SELECT 3, 'three' $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pairs_sql();",
                    expected: Expected::Rows {
                        columns: &[Column("pairs_sql", USER_DEFINED)],
                        rows: &[
                            &[T("(3,three)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION nested_do() RETURNS void AS $f$ BEGIN DO $b$ BEGIN INSERT INTO goods VALUES (3, 'cherry', 1, 9); END $b$; END; $f$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nested_do();",
                    expected: Expected::Rows {
                        columns: &[Column("nested_do", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM goods WHERE id = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("cherry")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Arrays of arrays",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ARRAY[ARRAY[]::int[]];",
                    expected: Expected::Rows {
                        columns: &[Column("array", INT4_ARRAY)],
                        rows: &[
                            &[T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY[ARRAY[1,2]::int[], ARRAY[3,4]::int[]];",
                    expected: Expected::Rows {
                        columns: &[Column("array", INT4_ARRAY)],
                        rows: &[
                            &[T("{{1,2},{3,4}}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_typeof(ARRAY[ARRAY[1]::int[]]);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("integer[]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Recreated serial sequences start over",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE rs (pk SERIAL PRIMARY KEY, v TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rs (v) VALUES ('a'), ('b');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rs;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rs (pk SERIAL PRIMARY KEY, v TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rs (v) VALUES ('c') RETURNING pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_updatable_views() {
    run_scripts(&[
        ScriptTest {
            name: "Automatically updatable views",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE uv_base (a INT PRIMARY KEY, b TEXT DEFAULT 'Unspecified');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uv_base SELECT i, 'Row ' || i FROM generate_series(-2, 2) g(i);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW uv_rw1 AS SELECT * FROM uv_base WHERE a > 0;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW uv_rw2 AS SELECT b AS bb, a AS aa, a + 1 AS ac FROM uv_base t WHERE t.a < 2;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW uv_ro1 AS SELECT DISTINCT a, b FROM uv_base;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW uv_ro2 AS SELECT count(*) FROM uv_base;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uv_rw1 VALUES (3, 'Row 3');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uv_rw1 (a) VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uv_rw1 SET a = 5 WHERE a = 4;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM uv_rw1 WHERE b = 'Row 2';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM uv_base ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("-2"), T("Row -2")],
                            &[T("-1"), T("Row -1")],
                            &[T("0"), T("Row 0")],
                            &[T("1"), T("Row 1")],
                            &[T("3"), T("Row 3")],
                            &[T("5"), T("Unspecified")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uv_rw2 SET bb = 'x' || bb WHERE aa < 1 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("bb", TEXT), Column("aa", INT4), Column("ac", INT4)],
                        rows: &[
                            &[T("xRow -2"), T("-2"), T("-1")],
                            &[T("xRow -1"), T("-1"), T("0")],
                            &[T("xRow 0"), T("0"), T("1")],
                        ],
                        tag: "UPDATE 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uv_rw2 SET ac = 1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot update column "ac" of view "uv_rw2""#, detail: "View columns that are not columns of their base relation are not updatable.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uv_rw2 (aa, bb) VALUES (10, 'ten') RETURNING aa, ac;",
                    expected: Expected::Rows {
                        columns: &[Column("aa", INT4), Column("ac", INT4)],
                        rows: &[
                            &[T("10"), T("11")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uv_rw2 VALUES ('q', 11, 12);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot insert into column "ac" of view "uv_rw2""#, detail: "View columns that are not columns of their base relation are not updatable.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM uv_rw2 v WHERE v.aa = -2 RETURNING v.*;",
                    expected: Expected::Rows {
                        columns: &[Column("bb", TEXT), Column("aa", INT4), Column("ac", INT4)],
                        rows: &[
                            &[T("xRow -2"), T("-2"), T("-1")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uv_ro1 VALUES (1, 'x');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot insert into view "uv_ro1""#, detail: "Views containing DISTINCT are not automatically updatable.", hint: "To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uv_ro2 SET count = 1;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot update view "uv_ro2""#, detail: "Views that return aggregate functions are not automatically updatable.", hint: "To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM uv_ro1;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot delete from view "uv_ro1""#, detail: "Views containing DISTINCT are not automatically updatable.", hint: "To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM uv_base ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("-1"), T("xRow -1")],
                            &[T("0"), T("xRow 0")],
                            &[T("1"), T("Row 1")],
                            &[T("3"), T("Row 3")],
                            &[T("5"), T("Unspecified")],
                            &[T("10"), T("ten")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Updatable view check options and information_schema",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE uvc_base (a INT PRIMARY KEY, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW uvc_rw1 AS SELECT * FROM uvc_base WHERE a > 0;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW uvc_rw3 AS SELECT * FROM uvc_rw1 WHERE a < 10 WITH CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uvc_rw3 VALUES (6, 'six');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uvc_rw3 VALUES (20, 'twenty');",
                    expected: Expected::Error(Diagnostic { code: "44000", message: r#"new row violates check option for view "uvc_rw3""#, detail: "Failing row contains (20, twenty).", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uvc_rw3 VALUES (-5, 'neg');",
                    expected: Expected::Error(Diagnostic { code: "44000", message: r#"new row violates check option for view "uvc_rw1""#, detail: "Failing row contains (-5, neg).", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE uvc_rw3 SET a = a + 100 WHERE a = 6;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: r#"new row violates check option for view "uvc_rw3""#, detail: "Failing row contains (106, six).", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW uvc_rw4 AS SELECT * FROM uvc_rw1 WHERE a < 10 WITH LOCAL CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uvc_rw4 VALUES (-6, 'neg');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM uvc_base ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("-6"), T("neg")],
                            &[T("6"), T("six")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW uvc_ro3 AS SELECT 1 AS one, a + 1 AS c FROM uvc_base;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, is_insertable_into FROM information_schema.tables WHERE table_name LIKE 'uvc_%' ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME), Column("is_insertable_into", VARCHAR)],
                        rows: &[
                            &[T("uvc_base"), T("YES")],
                            &[T("uvc_ro3"), T("NO")],
                            &[T("uvc_rw1"), T("YES")],
                            &[T("uvc_rw3"), T("YES")],
                            &[T("uvc_rw4"), T("YES")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, is_updatable, is_insertable_into, check_option FROM information_schema.views WHERE table_name LIKE 'uvc_%' ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME), Column("is_updatable", VARCHAR), Column("is_insertable_into", VARCHAR), Column("check_option", VARCHAR)],
                        rows: &[
                            &[T("uvc_ro3"), T("NO"), T("NO"), T("NONE")],
                            &[T("uvc_rw1"), T("YES"), T("YES"), T("NONE")],
                            &[T("uvc_rw3"), T("YES"), T("YES"), T("CASCADED")],
                            &[T("uvc_rw4"), T("YES"), T("YES"), T("LOCAL")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, is_updatable FROM information_schema.columns WHERE table_name LIKE 'uvc_r%' ORDER BY table_name, ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME), Column("column_name", NAME), Column("is_updatable", VARCHAR)],
                        rows: &[
                            &[T("uvc_ro3"), T("one"), T("NO")],
                            &[T("uvc_ro3"), T("c"), T("NO")],
                            &[T("uvc_rw1"), T("a"), T("YES")],
                            &[T("uvc_rw1"), T("b"), T("YES")],
                            &[T("uvc_rw3"), T("a"), T("YES")],
                            &[T("uvc_rw3"), T("b"), T("YES")],
                            &[T("uvc_rw4"), T("a"), T("YES")],
                            &[T("uvc_rw4"), T("b"), T("YES")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_view_privileges() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE VIEW checks table privileges only when the view is read",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE USER regress_cv_user;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA cv_schema;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON SCHEMA cv_schema TO regress_cv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE cv_schema.cv_base (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_cv_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW cv_schema.cv_view AS SELECT * FROM cv_schema.cv_base;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cv_schema.cv_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table cv_base", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_rule_select_rows() {
    run_scripts(&[
        ScriptTest {
            name: "rules that add SELECT statements send those statements' rows ahead of the command tag",
            set_up_script: &[
                "CREATE TABLE rt (a INT, b TEXT)",
                "INSERT INTO rt VALUES (1, 'one'), (2, 'two')",
                "CREATE VIEW rv AS SELECT a, b FROM rt",
                "CREATE RULE rv_ins AS ON INSERT TO rv DO INSTEAD SELECT new.a * 10 AS ten, new.b AS b",
                "CREATE RULE rv_upd AS ON UPDATE TO rv DO INSTEAD SELECT old.a, new.b AS nb",
                "CREATE RULE rv_del AS ON DELETE TO rv DO INSTEAD SELECT old.a AS gone",
                "CREATE RULE rt_also AS ON INSERT TO rt DO ALSO SELECT new.a + 100 AS also",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE pg_settings SET setting = '2' WHERE name = 'extra_float_digits'",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "UPDATE 0",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW extra_float_digits",
                    expected: Expected::Rows {
                        columns: &[Column("extra_float_digits", TEXT)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rv VALUES (3, 'three')",
                    expected: Expected::Rows {
                        columns: &[Column("ten", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("30"), T("three")],
                        ],
                        tag: "INSERT 0 0",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rv SET b = 'x' WHERE a = 1",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("nb", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "UPDATE 0",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rv WHERE a = 2",
                    expected: Expected::Rows {
                        columns: &[Column("gone", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "DELETE 0",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rv WHERE a = 5",
                    expected: Expected::Rows {
                        columns: &[Column("gone", INT4)],
                        rows: &[
                        ],
                        tag: "DELETE 0",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rt VALUES (4, 'four')",
                    expected: Expected::Rows {
                        columns: &[Column("also", INT4)],
                        rows: &[
                            &[T("104")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rt VALUES (5, 'five') RETURNING a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rt ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                            &[T("4"), T("four")],
                            &[T("5"), T("five")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p AS INSERT INTO rv VALUES (6, 'six')",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p",
                    expected: Expected::Rows {
                        columns: &[Column("ten", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("60"), T("six")],
                        ],
                        tag: "INSERT 0 0",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rv VALUES (7, 'seven')",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_with_check_options() {
    run_scripts(&[
        ScriptTest {
            name: "WITH CHECK OPTION through nested LOCAL and CASCADED views, partitions, moved rows, triggers, and subqueries",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE base (a INT PRIMARY KEY, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v1 AS SELECT * FROM base WHERE a > 0 WITH CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v1 VALUES (1, 10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v1 VALUES (-1, 10);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"v1\"", detail: "Failing row contains (-1, 10).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE v1 SET a = -a WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"v1\"", detail: "Failing row contains (-1, 10).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE v1 SET b = 11 WHERE a = 1 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v2 AS SELECT * FROM v1 WHERE b < 100;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v2 VALUES (2, 200);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v2 VALUES (-2, 20);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"v1\"", detail: "Failing row contains (-2, 20).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v3 AS SELECT * FROM v2 WHERE b > 5 WITH LOCAL CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v3 VALUES (3, 1);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"v3\"", detail: "Failing row contains (3, 1).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v3 VALUES (3, 500);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v3 VALUES (-3, 50);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"v1\"", detail: "Failing row contains (-3, 50).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v4 AS SELECT * FROM v2 WHERE b > 5 WITH CASCADED CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v4 VALUES (4, 500);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"v2\"", detail: "Failing row contains (4, 500).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v5 AS SELECT a, b FROM base WHERE b IS NOT NULL WITH CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v5 VALUES (5, NULL);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"v5\"", detail: "Failing row contains (5, null).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) INSERT INTO v1 VALUES (6, 6);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Insert on public.base")],
                            &[T("  ->  Result")],
                            &[T("        Output: 6, 6")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt (a INT, b TEXT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt1 PARTITION OF pt FOR VALUES IN (1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt2 (b TEXT, a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ATTACH PARTITION pt2 FOR VALUES IN (2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW pv AS SELECT * FROM pt WHERE b <> 'bad' WITH CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pv VALUES (1, 'ok'), (2, 'ok');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pv VALUES (2, 'bad');",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"pv\"", detail: "Failing row contains (2, bad).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pv SET b = 'bad' WHERE a = 2;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"pv\"", detail: "Failing row contains (2, bad).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pv SET a = 2, b = 'bad' WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"pv\"", detail: "Failing row contains (2, bad).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pv SET a = 2 WHERE a = 1 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("ok")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt2"), T("2"), T("ok")],
                            &[T("pt2"), T("2"), T("ok")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW vs AS SELECT * FROM base WHERE a IN (SELECT a FROM base WHERE b > 0) WITH CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vs VALUES (7, 1);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"vs\"", detail: "Failing row contains (7, 1).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION wco_trig() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.a := -NEW.a; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER wco_trig BEFORE INSERT ON base FOR EACH ROW EXECUTE FUNCTION wco_trig();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v1 VALUES (8, 8);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"v1\"", detail: "Failing row contains (-8, 8).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM base ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("200")],
                            &[T("3"), T("500")],
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
fn test_relation_and_column_updatability() {
    run_scripts(&[
        ScriptTest {
            name: "pg_relation_is_updatable and pg_column_is_updatable answer for tables, views, rules, and INSTEAD OF triggers, as information_schema shows",
            set_up_script: &[
                "CREATE TABLE b (a INT PRIMARY KEY, c TEXT, d INT);",
                "CREATE VIEW v1 AS SELECT a, c, d + 1 AS e FROM b;",
                "CREATE VIEW v2 AS SELECT e, a FROM v1;",
                "CREATE VIEW v3 AS SELECT DISTINCT a FROM b;",
                "CREATE VIEW v4 AS SELECT a + 1 AS x FROM b;",
                "CREATE VIEW v5 AS SELECT * FROM b;",
                "CREATE RULE v4_ins AS ON INSERT TO v4 DO INSTEAD NOTHING;",
                "CREATE FUNCTION tf() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN RETURN NULL; END $$;",
                "CREATE TRIGGER v3_upd INSTEAD OF UPDATE ON v3 FOR EACH ROW EXECUTE FUNCTION tf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT c.relname, pg_relation_is_updatable(c.oid, false), pg_relation_is_updatable(c.oid, true) FROM pg_class c WHERE relname IN ('b', 'v1', 'v2', 'v3', 'v4', 'v5') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("pg_relation_is_updatable", INT4), Column("pg_relation_is_updatable", INT4)],
                        rows: &[
                            &[T("b"), T("28"), T("28")],
                            &[T("v1"), T("28"), T("28")],
                            &[T("v2"), T("28"), T("28")],
                            &[T("v3"), T("0"), T("4")],
                            &[T("v4"), T("24"), T("24")],
                            &[T("v5"), T("28"), T("28")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, a.attname, pg_column_is_updatable(c.oid, a.attnum, false), pg_column_is_updatable(c.oid, a.attnum, true) FROM pg_class c JOIN pg_attribute a ON a.attrelid = c.oid WHERE relname IN ('v1', 'v2', 'v3', 'v4', 'v5') AND a.attnum > 0 ORDER BY 1, a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("attname", NAME), Column("pg_column_is_updatable", BOOL), Column("pg_column_is_updatable", BOOL)],
                        rows: &[
                            &[T("v1"), T("a"), T("t"), T("t")],
                            &[T("v1"), T("c"), T("t"), T("t")],
                            &[T("v1"), T("e"), T("f"), T("f")],
                            &[T("v2"), T("e"), T("f"), T("f")],
                            &[T("v2"), T("a"), T("t"), T("t")],
                            &[T("v3"), T("a"), T("f"), T("f")],
                            &[T("v4"), T("x"), T("f"), T("f")],
                            &[T("v5"), T("a"), T("t"), T("t")],
                            &[T("v5"), T("c"), T("t"), T("t")],
                            &[T("v5"), T("d"), T("t"), T("t")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, is_insertable_into, is_updatable, is_trigger_updatable, is_trigger_insertable_into FROM information_schema.views WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME), Column("is_insertable_into", VARCHAR), Column("is_updatable", VARCHAR), Column("is_trigger_updatable", VARCHAR), Column("is_trigger_insertable_into", VARCHAR)],
                        rows: &[
                            &[T("v1"), T("YES"), T("YES"), T("NO"), T("NO")],
                            &[T("v2"), T("YES"), T("YES"), T("NO"), T("NO")],
                            &[T("v3"), T("NO"), T("NO"), T("YES"), T("NO")],
                            &[T("v4"), T("YES"), T("NO"), T("NO"), T("NO")],
                            &[T("v5"), T("YES"), T("YES"), T("NO"), T("NO")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, is_updatable FROM information_schema.columns WHERE table_schema = 'public' ORDER BY 1, ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME), Column("column_name", NAME), Column("is_updatable", VARCHAR)],
                        rows: &[
                            &[T("b"), T("a"), T("YES")],
                            &[T("b"), T("c"), T("YES")],
                            &[T("b"), T("d"), T("YES")],
                            &[T("v1"), T("a"), T("YES")],
                            &[T("v1"), T("c"), T("YES")],
                            &[T("v1"), T("e"), T("NO")],
                            &[T("v2"), T("e"), T("NO")],
                            &[T("v2"), T("a"), T("YES")],
                            &[T("v3"), T("a"), T("NO")],
                            &[T("v4"), T("x"), T("NO")],
                            &[T("v5"), T("a"), T("YES")],
                            &[T("v5"), T("c"), T("YES")],
                            &[T("v5"), T("d"), T("YES")],
                        ],
                        tag: "SELECT 13",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_is_updatable(0, true), pg_column_is_updatable('b'::regclass, -1, true);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function pg_column_is_updatable(regclass, integer, boolean) does not exist", position: 43, hint: "No function matches the given name and argument types. You might need to add explicit type casts.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, pg_encoding_max_length(g) FROM generate_series(-1, 42) g WHERE g IN (-1, 0, 4, 6, 35, 41, 42);",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("pg_encoding_max_length", INT4)],
                        rows: &[
                            &[T("-1"), Null],
                            &[T("0"), T("1")],
                            &[T("4"), T("4")],
                            &[T("6"), T("4")],
                            &[T("35"), T("2")],
                            &[T("41"), T("2")],
                            &[T("42"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_materialized_views() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE MATERIALIZED VIEW fills the view, WITH NO DATA leaves it unpopulated, and REFRESH writes its rows again under new storage",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t VALUES (1, 'x'), (2, 'y'), (3, 'x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW mv AS SELECT b, count(*) AS n FROM t GROUP BY b;",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("n", INT8)],
                        rows: &[
                            &[T("x"), T("2")],
                            &[T("y"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relkind, relispopulated, relhasrules FROM pg_class WHERE relname = 'mv';",
                    expected: Expected::Rows {
                        columns: &[Column("relkind", CHAR), Column("relispopulated", BOOL), Column("relhasrules", BOOL)],
                        rows: &[
                            &[T("m"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('mv'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT b,
    count(*) AS n
   FROM t
  GROUP BY b;")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW mv2 (x) AS SELECT a FROM t WITH NO DATA;",
                    expected: Expected::Tag("CREATE MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv2;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "materialized view \"mv2\" has not been populated", hint: "Use the REFRESH MATERIALIZED VIEW command.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relispopulated FROM pg_class WHERE relname = 'mv2';",
                    expected: Expected::Rows {
                        columns: &[Column("relispopulated", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW mv2;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv2 ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 'z');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("n", INT8)],
                        rows: &[
                            &[T("x"), T("2")],
                            &[T("y"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE fn AS SELECT pg_relation_filenode('mv') AS f;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW mv;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("n", INT8)],
                        rows: &[
                            &[T("x"), T("2")],
                            &[T("y"), T("1")],
                            &[T("z"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_filenode('mv') <> f FROM fn;",
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
                    query: "CREATE UNIQUE INDEX mv_b ON mv (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW mv WITH NO DATA;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "materialized view \"mv\" has not been populated", hint: "Use the REFRESH MATERIALIZED VIEW command.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW mv;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv WHERE b = 'z';",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("n", INT8)],
                        rows: &[
                            &[T("z"), T("1")],
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
                    query: "INSERT INTO mv VALUES ('q', 1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot change materialized view \"mv\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW IF NOT EXISTS mv AS SELECT 1;",
                    notices: &[Diagnostic { code: "42P07", message: "relation \"mv\" already exists, skipping", ..N }],
                    expected: Expected::Tag("CREATE MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW mvdup AS SELECT b FROM t;",
                    expected: Expected::Tag("SELECT 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX mvdup_b ON mvdup (b);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"mvdup_b\"", detail: "Key (b)=(x) is duplicated.", schema: "public", table: "mvdup", constraint: "mvdup_b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP MATERIALIZED VIEW mv2;",
                    expected: Expected::Tag("DROP MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP MATERIALIZED VIEW t;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "\"t\" is not a materialized view", hint: "Use DROP TABLE to remove a table.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE mv;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "\"mv\" is not a table", hint: "Use DROP MATERIALIZED VIEW to remove a materialized view.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "materialized views refuse what Postgres refuses",
            set_up_script: &[
                "CREATE TABLE t (a INT, b TEXT);",
                "CREATE VIEW v AS SELECT 1 AS x;",
                "CREATE TEMP TABLE tt (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW m1 AS SELECT * FROM tt;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "materialized views must not use temporary tables or views", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNLOGGED MATERIALIZED VIEW m2 AS SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "materialized views cannot be unlogged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW m3 AS WITH d AS (DELETE FROM t RETURNING *) SELECT * FROM d;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "materialized views must not use data-modifying statements in WITH", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW t;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "\"t\" is not a materialized view", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW v;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "\"v\" is not a table or materialized view", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW mv AS SELECT a, b FROM t WITH NO DATA;",
                    expected: Expected::Tag("CREATE MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "CONCURRENTLY cannot be used when the materialized view is not populated", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW mv;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv WITH NO DATA;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "CONCURRENTLY and WITH NO DATA options cannot be used together", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot refresh materialized view \"public.mv\" concurrently", hint: "Create a unique index with no WHERE clause on one or more columns of the materialized view.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX mv_a ON mv (a) WHERE a > 0;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot refresh materialized view \"public.mv\" concurrently", hint: "Create a unique index with no WHERE clause on one or more columns of the materialized view.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE mv_user;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON mv TO mv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE mv_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW mv;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for materialized view mv", ..E }),
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
            name: "REFRESH MATERIALIZED VIEW CONCURRENTLY deletes and inserts only the changed rows, keeping the storage",
            set_up_script: &[
                "CREATE TABLE t (a INT, b TEXT);",
                "INSERT INTO t VALUES (1, 'x'), (2, 'y'), (3, NULL);",
                "CREATE MATERIALIZED VIEW mv AS SELECT a, b FROM t;",
                "CREATE UNIQUE INDEX mv_a ON mv (a);",
                "CREATE MATERIALIZED VIEW mv2 AS SELECT a, b FROM t;",
                "CREATE UNIQUE INDEX mv2_b ON mv2 (b);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE fn AS SELECT pg_relation_filenode('mv') AS f, pg_relation_filenode('mv_a') AS fi;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 'z');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET b = 'yy' WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE a = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("yy")],
                            &[T("3"), Null],
                            &[T("4"), T("z")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_filenode('mv') = f, pg_relation_filenode('mv_a') = fi FROM fn;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (5, 'd'), (5, 'd');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "new data for materialized view \"mv\" contains duplicate rows without any null columns", detail: "Row: (5,d)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE a = 5;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (6, NULL), (6, NULL);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"mv_a\"", detail: "Key (a)=(6) already exists.", schema: "public", table: "mv", constraint: "mv_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("yy")],
                            &[T("3"), Null],
                            &[T("4"), T("z")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE a = 6;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("yy")],
                            &[T("3"), Null],
                            &[T("4"), T("z")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (8, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv2;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mv2 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("yy")],
                            &[T("3"), Null],
                            &[T("4"), T("z")],
                            &[T("8"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REFRESH MATERIALIZED VIEW CONCURRENTLY creates the session's temporary schema, as Postgres runs its merge through temporary tables",
            set_up_script: &[
                "CREATE TABLE t (a INT, b TEXT);",
                "INSERT INTO t VALUES (1, 'x');",
                "CREATE MATERIALIZED VIEW mv AS SELECT a, b FROM t;",
                "CREATE UNIQUE INDEX mv_a ON mv (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_my_temp_schema() = 0;",
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
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_my_temp_schema() = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("REFRESH CONCURRENTLY merges through the table rather than SQL on temporary tables, since equal rows of a table without a primary key share one tuple id, so it makes no temporary schema"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_stored_rules_name_relation_columns() {
    run_scripts(&[
        ScriptTest {
            name: "a stored rule keeps a relation's column names as they were when it was made",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT);",
                "CREATE VIEW v AS SELECT a FROM t;",
                "ALTER TABLE t RENAME a TO c;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ev_action::text LIKE '%:colnames (\"a\" \"b\")%' FROM pg_rewrite WHERE ev_class = 'v'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("stored trees name a relation's columns by their ids, so a stored rule's relation entries show the columns' current names"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('v'::regclass, true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT c AS a\n   FROM t;")],
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
fn test_view_over_join_sorted_by_several_columns_keeps_names() {
    run_scripts(&[
        ScriptTest {
            name: "a view over a join sorted by several columns keeps its column names",
            set_up_script: &[
                "CREATE TABLE w (a int, b text, c int);",
                "INSERT INTO w VALUES (1, 'x', 2), (2, 'y', 3);",
                "CREATE VIEW wv2 AS SELECT w1.a AS p, w2.b AS q, w1.c AS r FROM w w1, LATERAL (SELECT * FROM w x WHERE x.a = w1.a) w2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM wv2 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("p", INT4), Column("q", TEXT), Column("r", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("2")],
                            &[T("2"), T("y"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wv2 ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("p", INT4), Column("q", TEXT), Column("r", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("2")],
                            &[T("2"), T("y"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p AS first, q AS second FROM wv2 ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("first", INT4), Column("second", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wv2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("p", INT4), Column("q", TEXT), Column("r", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("2")],
                            &[T("2"), T("y"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_check_options_of_views_over_a_view_with_instead_of_triggers() {
    run_scripts(&[
        ScriptTest {
            name: "a view's WITH CHECK OPTION checks the rows an INSTEAD OF trigger on the view under it returns",
            set_up_script: &[
                "CREATE TABLE base_tbl (a INT, b INT);",
                "CREATE VIEW rw_view1 AS SELECT a FROM base_tbl WHERE a < b;",
                "CREATE FUNCTION rw_view1_trig_fn() RETURNS trigger AS $$ BEGIN IF TG_OP = 'INSERT' THEN INSERT INTO base_tbl VALUES (NEW.a, 10); RETURN NEW; ELSIF TG_OP = 'UPDATE' THEN UPDATE base_tbl SET a = NEW.a WHERE a = OLD.a; RETURN NEW; END IF; RETURN OLD; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER rw_view1_trig INSTEAD OF INSERT OR UPDATE OR DELETE ON rw_view1 FOR EACH ROW EXECUTE PROCEDURE rw_view1_trig_fn();",
                "CREATE VIEW rw_view2 AS SELECT * FROM rw_view1 WHERE a > 0 WITH LOCAL CHECK OPTION;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO rw_view2 VALUES (-5);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"rw_view2\"", detail: "Failing row contains (-5).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO rw_view2 t USING (VALUES (-5)) AS v(a) ON t.a = v.a WHEN NOT MATCHED THEN INSERT VALUES (v.a);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"rw_view2\"", detail: "Failing row contains (-5).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rw_view2 VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rw_view2 VALUES (50);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rw_view2 SET a = a - 10;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"rw_view2\"", detail: "Failing row contains (-5).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO rw_view2 t USING (VALUES (5)) AS v(a) ON t.a = v.a WHEN MATCHED THEN UPDATE SET a = t.a - 10;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"rw_view2\"", detail: "Failing row contains (-5).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER VIEW rw_view2 SET (check_option = cascaded);",
                    expected: Expected::Tag("ALTER VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rw_view2 VALUES (100);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM base_tbl ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("5"), T("10")],
                            &[T("50"), T("10")],
                            &[T("100"), T("10")],
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
fn test_view_over_function_whose_row_type_changed() {
    run_scripts(&[
        ScriptTest {
            name: "a view over a function's rows fails to read a column dropped or retyped behind its back",
            set_up_script: &[
                "CREATE TABLE vrt_t (f1 text, f2 text, f3 text, f4 text)",
                "INSERT INTO vrt_t VALUES ('foo', 'bar', 'baz', '42')",
                "ALTER TABLE vrt_t DROP COLUMN f2",
                "CREATE FUNCTION vrt_f() RETURNS SETOF vrt_t AS $$ DECLARE rec1 record; BEGIN FOR rec1 IN SELECT * FROM vrt_t LOOP RETURN NEXT rec1; END LOOP; END; $$ LANGUAGE plpgsql",
                "CREATE VIEW vrt_v AS SELECT t.* FROM vrt_f() t",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pg_depend WHERE objid = (SELECT oid FROM pg_rewrite WHERE ev_class = 'vrt_v'::regclass AND rulename = '_RETURN') AND refobjsubid = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE vrt_t DROP COLUMN f3;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f1, f4 FROM vrt_v;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", TEXT), Column("f4", TEXT)],
                        rows: &[
                            &[T("foo"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vrt_v;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "attribute 3 of type record has been dropped", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pg_depend WHERE objid = (SELECT oid FROM pg_rewrite WHERE ev_class = 'vrt_v'::regclass AND rulename = '_RETURN') AND refobjsubid = 4;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE vrt_t ALTER COLUMN f4 TYPE integer USING f4::integer;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vrt_v;",
                    expected: Expected::Error(Diagnostic {
                        code: "42804",
                        message: "attribute 4 of type record has wrong type",
                        detail: "Table has type integer, but query expects text.",
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vrt_v;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", TEXT), Column("f3", TEXT), Column("f4", TEXT)],
                        rows: &[
                            &[T("foo"), T("baz"), T("42")],
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
fn test_refresh_concurrently_after_its_query_drops_the_unique_index() {
    run_scripts(&[
        ScriptTest {
            name: "REFRESH MATERIALIZED VIEW CONCURRENTLY whose query drops the view's unique index",
            set_up_script: &[
                "CREATE FUNCTION rcd_drop_the_index() RETURNS bool AS $$ BEGIN EXECUTE 'DROP INDEX IF EXISTS public.rcd_idx'; RETURN true; END; $$ LANGUAGE plpgsql",
                "CREATE MATERIALIZED VIEW rcd_matview AS SELECT 1 AS i WHERE rcd_drop_the_index()",
                "CREATE UNIQUE INDEX rcd_idx ON rcd_matview (i)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY rcd_matview;",
                    expected: Expected::Error(Diagnostic {
                        code: "0A000",
                        message: r#"could not find suitable unique index on materialized view "rcd_matview""#,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_indexes WHERE indexname = 'rcd_idx';",
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
    ]);
}

#[test]
fn test_view_over_function_keeps_its_column_names_after_a_rename() {
    run_scripts(&[
        ScriptTest {
            name: "a view over a function keeps the names the function's row type had when the view was made",
            set_up_script: &[
                "CREATE TYPE frn_t AS (a int, b int)",
                "CREATE FUNCTION frn_f() RETURNS SETOF frn_t LANGUAGE sql AS $$ SELECT 1, 2 $$",
                "CREATE VIEW frn_v AS SELECT * FROM frn_f()",
                "CREATE VIEW frn_v2 AS SELECT t.a, t.b FROM frn_f() t",
                "ALTER TYPE frn_t RENAME ATTRIBUTE a TO z",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('frn_v'), pg_get_viewdef('frn_v2');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT), Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT a,\n    b\n   FROM frn_f() frn_f(a, b);"), T(" SELECT a,\n    b\n   FROM frn_f() t(a, b);")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM frn_v;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on public.frn_f")],
                            &[T("  Output: frn_f.a, frn_f.b")],
                            &[T("  Function Call: frn_f()")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM frn_v2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on public.frn_f t")],
                            &[T("  Output: t.a, t.b")],
                            &[T("  Function Call: frn_f()")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM frn_v;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
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
fn test_views_keep_postgres_columns_after_their_relations_change() {
    run_scripts(&[
        ScriptTest {
            name: "views over a USING join keep Postgres' columns after a column is added to the joined table",
            set_up_script: &[
                "CREATE TABLE tt2 (a INT, b INT, c INT)",
                "CREATE TABLE tt3 (ax INT8, b INT2, c NUMERIC)",
                "INSERT INTO tt2 VALUES (1, 2, 3)",
                "INSERT INTO tt3 VALUES (10, 2, 3)",
                "CREATE VIEW v1 AS SELECT * FROM tt2 JOIN tt3 USING (b, c)",
                "CREATE VIEW v1a AS SELECT * FROM (tt2 JOIN tt3 USING (b, c)) j",
                "CREATE VIEW v1b AS SELECT * FROM tt3 AS y(z) JOIN tt2 USING (b, c)",
                "ALTER TABLE tt2 ADD COLUMN ax INT",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('v1', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT tt2.b,\n    tt3.c,\n    tt2.a,\n    tt3.ax\n   FROM tt2\n     JOIN tt3 USING (b, c);")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('v1a', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT j.b,\n    j.c,\n    j.a,\n    j.ax\n   FROM (tt2\n     JOIN tt3 USING (b, c)) j(b, c, a, ax_1, ax);")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('v1b', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT tt2.b,\n    y.c,\n    y.z,\n    tt2.a\n   FROM tt3 y(z, b, c)\n     JOIN tt2 USING (b, c);")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1a;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("c", NUMERIC), Column("a", INT4), Column("ax", INT8)],
                        rows: &[
                            &[T("2"), T("3"), T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a view over a USING join keeps Postgres' columns after a column is dropped and added again under its name",
            set_up_script: &[
                "CREATE TABLE tt3 (ax INT8, b INT2, c NUMERIC)",
                "INSERT INTO tt3 VALUES (10, 2, 3)",
                "CREATE TABLE tt4 (a INT, b INT, c INT, e INT)",
                "INSERT INTO tt4 VALUES (1, 2, 3, 4)",
                "CREATE VIEW v4 AS SELECT j.a, j.ax FROM (tt4 JOIN tt3 USING (b, c)) j",
                "ALTER TABLE tt4 DROP COLUMN e",
                "ALTER TABLE tt4 ADD COLUMN e INT",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('v4', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT j.a,\n    j.ax\n   FROM (tt4\n     JOIN tt3 USING (b, c)) j;")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("ax", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "views with whole-row references keep Postgres' columns after a column is added and dropped",
            set_up_script: &[
                "CREATE TABLE w (a INT, b INT)",
                "CREATE TABLE w2 (a INT, d INT)",
                "INSERT INTO w VALUES (1, 2)",
                "INSERT INTO w2 VALUES (1, 5)",
                "CREATE VIEW vw1 AS SELECT w FROM w",
                "CREATE VIEW vw2 AS SELECT w.*, w2.d FROM w JOIN w2 USING (a)",
                "CREATE VIEW vw3 AS SELECT w FROM w JOIN w2 USING (a)",
                "ALTER TABLE w ADD COLUMN c INT",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('vw1', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT w.*::w AS w\n   FROM w;")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('vw2', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT w.a,\n    w.b,\n    w2.d\n   FROM w\n     JOIN w2 USING (a);")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('vw3', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT w.*::w AS w\n   FROM w\n     JOIN w2 USING (a);")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vw1;",
                    expected: Expected::Rows {
                        columns: &[Column("w", USER_DEFINED)],
                        rows: &[
                            &[T("(1,2,)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vw2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vw3;",
                    expected: Expected::Rows {
                        columns: &[Column("w", USER_DEFINED)],
                        rows: &[
                            &[T("(1,2,)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE w DROP COLUMN c;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vw1;",
                    expected: Expected::Rows {
                        columns: &[Column("w", USER_DEFINED)],
                        rows: &[
                            &[T("(1,2)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vw3;",
                    expected: Expected::Rows {
                        columns: &[Column("w", USER_DEFINED)],
                        rows: &[
                            &[T("(1,2)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('vw3', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT w.*::w AS w\n   FROM w\n     JOIN w2 USING (a);")],
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
fn test_a_merged_view_reads_as_one_made_before_its_relation_changed() {
    run_scripts(&[
        ScriptTest {
            name: "a view over a USING join merged in from a branch reads as the same view made on main before the column was added",
            set_up_script: &[
                "CREATE TABLE tt2 (a INT, b INT, c INT)",
                "CREATE TABLE tt3 (ax INT8, b INT2, c NUMERIC)",
                "INSERT INTO tt2 VALUES (1, 2, 3)",
                "INSERT INTO tt3 VALUES (10, 2, 3)",
                "SELECT dolt_commit('-Am', 'tables')",
                "SELECT dolt_checkout('-b', 'feature')",
                "CREATE VIEW v1a AS SELECT * FROM (tt2 JOIN tt3 USING (b, c)) j",
                "SELECT dolt_commit('-Am', 'view')",
                "SELECT dolt_checkout('main')",
                "ALTER TABLE tt2 ADD COLUMN ax INT",
                "SELECT dolt_commit('-Am', 'column')",
                "SELECT dolt_merge('feature')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('v1a', true);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT j.b,\n    j.c,\n    j.a,\n    j.ax\n   FROM (tt2\n     JOIN tt3 USING (b, c)) j(b, c, a, ax_1, ax);")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1a;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("c", NUMERIC), Column("a", INT4), Column("ax", INT8)],
                        rows: &[
                            &[T("2"), T("3"), T("1"), T("10")],
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
fn test_refresh_concurrently_with_a_predicate_that_folds_to_true() {
    run_scripts(&[
        ScriptTest {
            name: "REFRESH MATERIALIZED VIEW CONCURRENTLY uses a unique index whose predicate folds to true, as Postgres judges an index by its folded predicate",
            set_up_script: &[
                "CREATE FUNCTION mvtrue(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN 1; END $$;",
                "CREATE MATERIALIZED VIEW mvf AS SELECT 1 AS c;",
                "CREATE UNIQUE INDEX ON mvf (c) WHERE mvtrue(1) > 0;",
                "CREATE MATERIALIZED VIEW mvp AS SELECT 1 AS c;",
                "CREATE UNIQUE INDEX ON mvp (c) WHERE c > 0;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mvf;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW CONCURRENTLY mvp;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot refresh materialized view \"public.mvp\" concurrently", hint: "Create a unique index with no WHERE clause on one or more columns of the materialized view.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
