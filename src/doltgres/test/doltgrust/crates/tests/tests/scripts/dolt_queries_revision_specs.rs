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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_revision_specs.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_revision_db_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "database revision specs: Ancestor references",
            intent: "A revision database or qualifier names a branch or tag with ~ and ^ ancestor steps, reads that commit's tables, and fails for an ancestor that doesn't exist or a commit that predates the table.",
            set_up_script: &[
                "CREATE TABLE t01 (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('t01');",
                "SELECT dolt_commit('-am', 'creating table t01 on main');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t01 VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-am', 'adding rows to table t01 on main');",
                "INSERT INTO t01 VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'adding another row to table t01 on main');",
                "SELECT dolt_tag('tag1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t01 VALUES (100, 100), (200, 200);",
                "SELECT dolt_commit('-am', 'inserting rows in t01 on branch1');",
                "INSERT INTO t01 VALUES (1000, 1000);",
                "SELECT dolt_commit('-am', 'inserting another row in t01 on branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    dolt: Some(Dolt::Differs("current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/tag1^\".public.t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/tag1^2\".public.t01;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/tag1^2\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Dolt fails a revision database of an invalid ancestor with \"invalid ancestor spec\", where Doltgrust fails it as a connection to that database fails, a missing database")),
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/tag1~1\".public.t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/tag1~2\".public.t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/tag1~3\".public.t01;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"public.t01\" does not exist", ..E }),
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/tag1~20\".public.t01;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/tag1~20\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Dolt fails a revision database of an invalid ancestor with \"invalid ancestor spec\", where Doltgrust fails it as a connection to that database fails, a missing database")),
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch1~\".public.t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("100"), T("100")],
                            &[T("200"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch1^\".public.t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("100"), T("100")],
                            &[T("200"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch1~2\".public.t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch1~3\".public.t01;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"public.t01\" does not exist", ..E }),
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD^0') = dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust takes a revision ending in `^0` as the commit itself, as Git does, where Dolt refuses it, by the owner's decision 2026-10-27 (Appendix C)")),
                    client: "c1",
                    database: "postgres/tag1~",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "database revision specs: tag-qualified revision spec",
            intent: "A connection to a tag's revision database reads the tag's commit read-only, with no active branch, until dolt_checkout switches it to a branch.",
            set_up_script: &[
                "CREATE TABLE t01 (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table t01 on main');",
                "INSERT INTO t01 VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-am', 'adding rows to table t01 on main');",
                "SELECT dolt_tag('tag1');",
                "INSERT INTO t01 VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'adding another row to table t01 on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/tag1",
                    dolt: Some(Dolt::Differs("current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/tag1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/tag1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres/tag1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute dolt_reset() in a read-only transaction", ..E }),
                    client: "c1",
                    database: "postgres/tag1",
                    dolt: Some(Dolt::Differs("Postgres refuses dolt_reset() in a read-only transaction with its own text, where Dolt names the read-only revision database (Appendix C \"revision connections\")")),
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
                    client: "c1",
                    database: "postgres/tag1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/tag1",
                    dolt: Some(Dolt::Differs("a connection's database never changes, so dolt_checkout switches its branch and current_database() stays the name it connected with, Appendix C \"USE db\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/tag1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "database revision specs: branch-qualified revision spec",
            intent: "A connection to a branch's revision database works on that branch, its tables included, and a table created there is in that branch's working set, which a connection that checks out the branch sees.",
            set_up_script: &[
                "CREATE TABLE t01 (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table t01 on main');",
                "INSERT INTO t01 VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-am', 'adding rows to table t01 on main');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t01 VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'adding another row to table t01 on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::Differs("current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres/branch1",
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
                    client: "c1",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::Differs("a connection's database never changes, so dolt_checkout switches its branch and current_database() stays the name it connected with, Appendix C \"USE db\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::Differs("current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE working_set_table (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "c2",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("working_set_table")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres/branch1",
                    skip: Some("dolt_diff names the table by its schema-qualified name, public.working_set_table, where the expectation has Dolt's bare name"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("working_set_table")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("dolt_diff names the table by its schema-qualified name, public.working_set_table, where the expectation has Dolt's bare name"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "database revision specs: dolt_checkout uses revision database name for DbData access",
            intent: "dolt_checkout and dolt_branch work from a connection to another database and to its revision databases.",
            set_up_script: &[
                "CREATE DATABASE newtest;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "newtest",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'branch-to-delete');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch-to-delete'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "newtest",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch-to-delete")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "newtest",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "newtest/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'branch-to-delete');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "newtest/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "newtest/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'another-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'another-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "newtest/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("another-branch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "newtest/main",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "database revision specs: can checkout a table",
            intent: "dolt_checkout of a table works from a connection to a branch's revision database.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE t01 (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('t01');",
                "SELECT dolt_commit('-am', 'creating table t01 on branch1');",
                "INSERT INTO t01 VALUES (1, 1), (2, 2);",
                "SELECT dolt_branch('new-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t01"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/main",
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('t01');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/main",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "database revision specs: prisma detects existing _prisma_migrations on branch",
            intent: "Prisma's query listing a schema's base tables finds a table created on a branch from a connection to that branch, through either revision delimiter.",
            set_up_script: &[
                "CREATE TABLE t01 (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('newbranch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.schema', current_schema(), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres@newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE stmt_list_base_tables AS SELECT DISTINCT table_info.table_name AS table_name FROM information_schema.tables AS table_info JOIN information_schema.columns AS column_info ON column_info.table_name = table_info.table_name WHERE table_info.table_schema = $1 AND column_info.table_schema = $2 AND table_info.table_type = 'BASE TABLE' ORDER BY table_info.table_name;",
                    expected: Expected::Tag("PREPARE"),
                    client: "c1",
                    database: "postgres@newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE _prisma_migrations (id VARCHAR(36) PRIMARY KEY NOT NULL, checksum VARCHAR(64) NOT NULL, finished_at timestamp(3), migration_name VARCHAR(255) NOT NULL, logs TEXT, rolled_back_at timestamp(3), started_at timestamp(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3), applied_steps_count integer NOT NULL DEFAULT 0);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "c1",
                    database: "postgres@newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, checksum, finished_at, migration_name, logs, rolled_back_at, started_at, applied_steps_count FROM _prisma_migrations ORDER BY started_at ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR), Column("checksum", VARCHAR), Column("finished_at", TIMESTAMP), Column("migration_name", VARCHAR), Column("logs", TEXT), Column("rolled_back_at", TIMESTAMP), Column("started_at", TIMESTAMP), Column("applied_steps_count", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c1",
                    database: "postgres@newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt_list_base_tables(current_setting('test.schema'), current_setting('test.schema'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("_prisma_migrations")],
                            &[T("t01")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres@newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_show_branch_databases = ON;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "unrecognized configuration parameter \"dolt_show_branch_databases\"", ..E }),
                    client: "c1",
                    database: "postgres@newbranch",
                    dolt: Some(Dolt::Differs("Dolt's setting lists revision databases beside databases, and a revision of a Doltgrust database is never a database of its own, so pg_database lists it once, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt_list_base_tables(current_setting('test.schema'), current_setting('test.schema'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("_prisma_migrations")],
                            &[T("t01")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres@newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres/newbranch",
                    dolt: Some(Dolt::Differs("current_database() names the base database, as pg_database does, and current_branch() names the revision, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.schema', current_schema(), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres/newbranch",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE keeps the session's variables and prepared statements, and a new connection sets and prepares them again")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE stmt_list_base_tables AS SELECT DISTINCT table_info.table_name AS table_name FROM information_schema.tables AS table_info JOIN information_schema.columns AS column_info ON column_info.table_name = table_info.table_name WHERE table_info.table_schema = $1 AND column_info.table_schema = $2 AND table_info.table_type = 'BASE TABLE' ORDER BY table_info.table_name;",
                    expected: Expected::Tag("PREPARE"),
                    client: "c2",
                    database: "postgres/newbranch",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE keeps the session's variables and prepared statements, and a new connection sets and prepares them again")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt_list_base_tables(current_setting('test.schema'), current_setting('test.schema'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("_prisma_migrations")],
                            &[T("t01")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c2",
                    database: "postgres/newbranch",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "database revision specs: db revision delimiter alias '@'",
            intent: "@ separates a database from its revision as / does, in connections and qualifiers, a database can't be created with it in its name, and writes through a qualifier reach that branch.",
            set_up_script: &[
                "CREATE TABLE t02 (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_commit('-Am', 'creating table t02 on main');",
                "SELECT dolt_tag('tag2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE \"postgres@branch1\";",
                    expected: Expected::Error(Diagnostic { code: "42602", message: "unacceptable database name \"postgres@branch1\"", detail: "The characters \"/\" and \"@\" mark a revision of a database.", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's refusal of a database name holding a revision mark takes Postgres' wording for unacceptable names, with a detail")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres@main",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres@main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t01 (pk int PRIMARY KEY, c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "c1",
                    database: "postgres@main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'creating table t01 on branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres@main",
                    dolt: Some(Dolt::Differs("Doltgrust's hashes are its own format's addresses, so they never equal Dolt's, Appendix C \"commit and table hashes\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    dolt: Some(Dolt::Differs("current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_show_branch_databases = ON;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "unrecognized configuration parameter \"dolt_show_branch_databases\"", ..E }),
                    client: "c2",
                    database: "postgres@branch1",
                    dolt: Some(Dolt::Differs("Dolt's setting lists revision databases beside databases, and a revision of a Doltgrust database is never a database of its own, so pg_database lists it once, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@branch1\".public.t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres@branch1\".public.t01 VALUES (1, 10), (2, 20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@branch1\".public.t01 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres@branch1\".public.t01 SET c1 = 30 WHERE pk = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@branch1\".public.t01 WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("2"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres@branch1\".public.t01 WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@branch1\".public.t01 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("2"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_t01_c1 ON \"postgres@branch1\".public.t01 (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass::text AS index_name, a.attname AS column_name FROM pg_index i JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY (i.indkey) WHERE i.indexrelid = 'idx_t01_c1'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("index_name", TEXT), Column("column_name", NAME)],
                        rows: &[
                            &[T("idx_t01_c1"), T("c1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    dolt: Some(Dolt::MySqlOnly("information_schema.statistics is MySQL's, so the port reads the index's columns from pg_index")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("2"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 't01' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 't01';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE parent (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE child (id int PRIMARY KEY, pid int, FOREIGN KEY (pid) REFERENCES \"postgres@branch1\".public.parent (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO parent VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "c2",
                    database: "postgres@branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::Differs("current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("2"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 't01' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 't01';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("pid", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE \"postgres@branch1\".public.child;",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "c3",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'child';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@tag2\".public.t02;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c3",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("2"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c4",
                    database: "postgres/branch1",
                    ..A
                },
            ],
                        ..S
        },
        ScriptTest {
            name: "database revision specs: commit id with revision delimiter alias '@'",
            intent: "A commit's hash names a revision to read.",
            set_up_script: &[
                "CREATE TABLE t01 (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.h1', dolt_commit('-am', 'creating table t01 on main'), false);",
                "INSERT INTO t01 VALUES (1, 1), (2, 2);",
                "SELECT set_config('test.h2', dolt_commit('-am', 'adding rows to table t01 on main'), false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t01 AS OF current_setting('test.h1');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("the script reaches the commit through a database named by its hash, which a client's connection can't name from a value the script computes, so the port reads the commit AS OF the hash")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01 AS OF current_setting('test.h2');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("the script reaches the commit through a database named by its hash, which a client's connection can't name from a value the script computes, so the port reads the commit AS OF the hash")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "database revision specs: delimiter conflicts in branch names",
            intent: "Branch names holding / or @ still name their branch in a revision database's name, however its delimiters could split it.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'branch@');",
                "CREATE TABLE t01 (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_commit('-Am', 'creating table t01 on `branch@`');",
                "SELECT * FROM dolt_checkout('-b', 'elian/test', 'main');",
                "CREATE TABLE t02 (pk int PRIMARY KEY, c2 int);",
                "SELECT dolt_commit('-Am', 'creating table t02 on `elian/test`');",
                "SELECT * FROM dolt_checkout('-b', 'elian/test@', 'main');",
                "CREATE TABLE t03 (pk int PRIMARY KEY, c3 int);",
                "SELECT dolt_commit('-Am', 'creating table t03 on `elian/test@`');",
                "SELECT * FROM dolt_checkout('-b', 'elian@test@', 'main');",
                "CREATE TABLE t04 (pk int PRIMARY KEY, c4 int);",
                "SELECT dolt_commit('-Am', 'creating table t04 on `elian@test@`');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch@');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch@'")],
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
                            &[T("branch@")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres@branch@",
                    dolt: Some(Dolt::Differs("current_database() names the base database, as pg_database does, and current_branch() names the revision, Appendix C \"revision connections\"; mySQL's database names ignore case, and Postgres' don't, so the port connects to the lowercase name")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c1",
                    database: "postgres@branch@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@branch@\".public.t01;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c1",
                    database: "postgres@branch@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 't01';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres@branch@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t02;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t02\" does not exist", position: 15, ..E }),
                    client: "c1",
                    database: "postgres@branch@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@elian/test",
                    dolt: Some(Dolt::Differs("current_database() names the base database, as pg_database does, and current_branch() names the revision, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("elian/test")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@elian/test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t02;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c2",
                    database: "postgres@elian/test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@elian/test\".public.t02;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c2",
                    database: "postgres@elian/test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 't02';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t02")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c2",
                    database: "postgres@elian/test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t03;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t03\" does not exist", position: 15, ..E }),
                    client: "c2",
                    database: "postgres@elian/test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres@elian/test@",
                    dolt: Some(Dolt::Differs("current_database() names the base database, as pg_database does, and current_branch() names the revision, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("elian/test@")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres@elian/test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t03;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c3", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c3",
                    database: "postgres@elian/test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@elian/test@\".public.t03;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c3", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c3",
                    database: "postgres@elian/test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 't03';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t03")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c3",
                    database: "postgres@elian/test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t04;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t04\" does not exist", position: 15, ..E }),
                    client: "c3",
                    database: "postgres@elian/test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c4",
                    database: "postgres@elian@test@",
                    dolt: Some(Dolt::Differs("current_database() names the base database, as pg_database does, and current_branch() names the revision, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("elian@test@")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c4",
                    database: "postgres@elian@test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t04;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c4", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c4",
                    database: "postgres@elian@test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres@elian@test@\".public.t04;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c4", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "c4",
                    database: "postgres@elian@test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 't04';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t04")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c4",
                    database: "postgres@elian@test@",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t01;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t01\" does not exist", position: 15, ..E }),
                    client: "c4",
                    database: "postgres@elian@test@",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_revision_qualifiers_that_name_nothing_or_do_not_parse() {
    run_scripts(&[
        ScriptTest {
            name: "a revision qualifier or connection naming nothing is a missing database, and one whose steps don't parse keeps Dolt's error",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 't');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/nosuch\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main~9\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/main~9\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main~x\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid HEAD spec: ~x", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main^x\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid HEAD spec: ^x", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'main~x';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid HEAD spec: ~x", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'nosuch';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: nosuch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::ClientError("FATAL: Invalid HEAD spec: ~x (SQLSTATE 22023)"),
                    client: "malformed",
                    database: "postgres/main~x",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::ClientError("FATAL: database \"postgres/main~9\" does not exist (SQLSTATE 3D000)"),
                    client: "deep",
                    database: "postgres/main~9",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
