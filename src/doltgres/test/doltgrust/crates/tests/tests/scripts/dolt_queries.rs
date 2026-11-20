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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, Dolt, E, Expected, N, PRIMARY_KEY_RETURNED, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_hashof_table tests",
            intent: "dolt_hashof_table gives a table's hash, which changes when the table's rows change, and fails for a table that doesn't exist.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_table('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_hashof_table", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust's hashes are its own format's addresses, so they never equal Dolt's, Appendix C \"commit and table hashes\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_table('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_hashof_table", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust's hashes are its own format's addresses, so they never equal Dolt's, Appendix C \"commit and table hashes\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_table('noexist');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"noexist\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_hashof_db tests",
            intent: "dolt_hashof_db gives the root hash of the working set by default and of a named revision otherwise, and each moves only when its own root changes.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY);",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "CREATE TABLE t3 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am','table creation commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.hashofdb', dolt_hashof_db(), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('HEAD');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('STAGED');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('WORKING');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('main');",
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
                    query: "SELECT dolt_checkout('-b','new');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"Switched to branch 'new'\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('new');",
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
                    query: "INSERT INTO t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db();",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('HEAD');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('STAGED');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('main');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('new');",
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
                    query: "SELECT set_config('test.hashofdb', dolt_hashof_db(), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('STAGED');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('STAGED');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('new');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added some rows to branch `new`');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('HEAD');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('new');",
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
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db('main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db();",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.hashofdb', dolt_hashof_db(), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE proc1() LANGUAGE sql AS 'SELECT * FROM t3';",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hashofdb') = dolt_hashof_db();",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
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
            name: "test hashof",
            intent: "hashof names a commit by its hash, a branch, HEAD, or HEAD~n, and refuses a revision that names nothing.",
            set_up_script: &[
                "CREATE TABLE hashof_test (pk int PRIMARY KEY, c1 int);",
                "INSERT INTO hashof_test VALUES (1,1), (2,2), (3,3);",
                "SELECT dolt_add('hashof_test');",
                "SELECT dolt_commit('-a', '-m', 'first commit');",
                "SELECT set_config('test.commit1', (SELECT commit_hash FROM dolt_log() LIMIT 1), false);",
                "INSERT INTO hashof_test VALUES (4,4), (5,5), (6,6);",
                "SELECT dolt_commit('-a', '-m', 'second commit');",
                "SELECT set_config('test.commit2', (SELECT commit_hash FROM dolt_log() LIMIT 1), false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT (hashof(current_setting('test.commit1')) = hashof(current_setting('test.commit2')));",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (hashof(current_setting('test.commit1')) = hashof('HEAD~1'));",
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
                    query: "SELECT (hashof(current_setting('test.commit2')) = hashof('HEAD'));",
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
                    query: "SELECT (hashof(current_setting('test.commit2')) = hashof('main'));",
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
                    query: "SELECT hashof('non_branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ref spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hashof(left(current_setting('test.commit2'),30)) = current_setting('test.commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust accepts an unambiguous prefix of a commit hash, Appendix C \"commit and table hashes\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status_ignored basic tests",
            intent: "dolt_status_ignored lists each change with whether dolt_ignore ignores its table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO dolt_ignore VALUES ('ignored_*', TRUE);",
                "CREATE TABLE ignored_test (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'dolt_status_ignored' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("table_name"), T("text"), T("YES")],
                            &[T("staged"), T("boolean"), T("YES")],
                            &[T("status"), T("text"), T("YES")],
                            &[T("ignored"), T("boolean"), T("YES")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so information_schema.columns describes the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status, ignored FROM dolt_status_ignored WHERE table_name = 'public.t';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.t"), T("f"), T("new table"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status, ignored FROM dolt_status_ignored WHERE table_name = 'public.ignored_test';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.ignored_test"), T("f"), T("new table"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE table_name = 'dolt_ignore';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("dolt_ignore"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status_ignored staged tables always have ignored=0",
            intent: "A staged table is never marked ignored.",
            set_up_script: &[
                "CREATE TABLE staged_test (pk int PRIMARY KEY);",
                "SELECT dolt_add('staged_test');",
                "INSERT INTO dolt_ignore VALUES ('staged_*', TRUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, ignored FROM dolt_status_ignored WHERE table_name = 'public.staged_test';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.staged_test"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status_ignored with AS OF and branch queries",
            intent: "dolt_status_ignored AS OF a tag or commit is empty, and AS OF a branch reads that branch's changes.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'empty commit');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_tag('tag1');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "CREATE TABLE abc (pk int);",
                "SELECT dolt_add('abc');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE t (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status, ignored FROM dolt_status_ignored;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.t"), T("f"), T("new table"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status_ignored AS OF 'tag1';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status_ignored AS OF current_setting('test.commit1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status, ignored FROM dolt_status_ignored AS OF 'branch1';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.abc"), T("t"), T("new table"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status, ignored FROM \"postgres/branch1\".pg_catalog.dolt_status_ignored;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.abc"), T("t"), T("new table"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status_ignored with multiple ignore patterns",
            intent: "Each of several ignore patterns marks its tables ignored.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('temp_*', TRUE);",
                "INSERT INTO dolt_ignore VALUES ('*_backup', TRUE);",
                "CREATE TABLE temp_data (pk int PRIMARY KEY);",
                "CREATE TABLE users_backup (pk int PRIMARY KEY);",
                "CREATE TABLE normal_table (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE table_name = 'public.temp_data';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.temp_data"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE table_name = 'public.users_backup';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.users_backup"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE table_name = 'public.normal_table';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.normal_table"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status_ignored with empty dolt_ignore",
            intent: "With no ignore patterns, no table is ignored.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY);",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.t1"), T("f")],
                            &[T("public.t2"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status_ignored with conflicting patterns",
            intent: "A pattern that excludes a table wins over one that ignores it.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('test_*', TRUE);",
                "INSERT INTO dolt_ignore VALUES ('test_special', FALSE);",
                "CREATE TABLE test_normal (pk int PRIMARY KEY);",
                "CREATE TABLE test_special (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE table_name = 'public.test_normal';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.test_normal"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE table_name = 'public.test_special';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.test_special"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status reports AUTO_INCREMENT value changes",
            intent: "Restarting an identity column's sequence is a change to its table that stages and commits.",
            set_up_script: &[
                "CREATE TABLE ainc (id int NOT NULL GENERATED BY DEFAULT AS IDENTITY, PRIMARY KEY (id));",
                "INSERT INTO ainc DEFAULT VALUES;",
                "SELECT dolt_commit('-Am', 'create ainc');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ainc ALTER COLUMN id RESTART WITH 100;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.ainc"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('ainc');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.ainc"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'bump auto_increment');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "ALTER TABLE ainc ALTER COLUMN id RESTART WITH 100;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
            name: "dolt_commit with only ignored table changes closes the transaction",
            intent: "dolt_commit with only ignored changes fails with nothing to commit.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignored_*', TRUE);",
                "SELECT dolt_add('dolt_ignore');",
                "SELECT dolt_commit('-m', 'set up ignore rules');",
                "START TRANSACTION;",
                "CREATE TABLE ignored_t1 (pk int PRIMARY KEY);",
                "INSERT INTO ignored_t1 VALUES (42);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'commit ignored changes');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ignored_t1 ORDER BY pk;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"ignored_t1\" does not exist", position: 15, ..E }),
                    client: "session1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff.from_commit test",
            intent: "dolt_diff's from commit names the parent commit of each change.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added test table');",
                "INSERT INTO test (pk, c1) VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'add rows 1-3');",
                "UPDATE  test SET c1=4 WHERE pk=2;",
                "UPDATE  test SET c1=5 WHERE pk=3;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'modified');",
                "UPDATE test SET c1=2 WHERE pk=2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff_test WHERE dolt_to_commit='WORKING' AND dolt_from_commit=dolt_hashof('main') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("2"), T("2"), T("4"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff_test WHERE dolt_from_commit=dolt_hashof('main') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("2"), T("2"), T("4"), T("modified")],
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
            name: "multiple unresolved foreign keys can be created on the same table",
            intent: "A table can be created with several foreign keys at once while foreign key checks are off.",
            set_up_script: &[
                "SET session_replication_role = replica;",
                "CREATE TABLE t1(pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (pk int PRIMARY KEY, c1 int, c2 int, FOREIGN KEY (\"c1\") REFERENCES \"t1\" (\"pk\") ON DELETE CASCADE ON UPDATE CASCADE, FOREIGN KEY (\"c2\") REFERENCES \"t1\" (\"pk\") ON DELETE CASCADE ON UPDATE CASCADE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test has_ancestor",
            intent: "has_ancestor tells whether one commit is an ancestor of another, by branch, tag, or hash.",
            set_up_script: &[
                "CREATE TABLE xy (x int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'create');",
                "SELECT set_config('test.main1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO xy VALUES (0);",
                "SELECT dolt_commit('-Am', 'add 0');",
                "SELECT set_config('test.main2', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_branch('bone', current_setting('test.main1', TRUE));",
                "SELECT * FROM dolt_checkout('bone');",
                "INSERT INTO xy VALUES (1);",
                "SELECT dolt_commit('-Am', 'add 1');",
                "SELECT set_config('test.bone1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO xy VALUES (2);",
                "SELECT dolt_commit('-Am', 'add 2');",
                "SELECT set_config('test.bone2', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_branch('btwo', current_setting('test.main1', TRUE));",
                "SELECT * FROM dolt_checkout('btwo');",
                "INSERT INTO xy VALUES (3);",
                "SELECT dolt_commit('-Am', 'add 3');",
                "SELECT set_config('test.btwo1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_tag('tag_btwo1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO xy VALUES (4);",
                "SELECT dolt_commit('-Am', 'add 4');",
                "SELECT set_config('test.main3', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_branch('onetwo', current_setting('test.bone2', TRUE));",
                "SELECT * FROM dolt_checkout('onetwo');",
                "SELECT * FROM dolt_merge('btwo');",
                "SELECT set_config('test.onetwo1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('bone');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT has_ancestor('main', current_setting('test.main1', TRUE)), has_ancestor('main', current_setting('test.main2', TRUE)), has_ancestor('main', current_setting('test.bone1', TRUE)), has_ancestor('main', current_setting('test.bone2', TRUE)), has_ancestor('main', current_setting('test.btwo1', TRUE)), has_ancestor('main', current_setting('test.onetwo1', TRUE)), has_ancestor('main', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_ancestor('bone', current_setting('test.main1', TRUE)), has_ancestor('bone', current_setting('test.main2', TRUE)), has_ancestor('bone', current_setting('test.bone1', TRUE)), has_ancestor('bone', current_setting('test.bone2', TRUE)), has_ancestor('bone', current_setting('test.btwo1', TRUE)), has_ancestor('bone', current_setting('test.onetwo1', TRUE)), has_ancestor('bone', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), T("t"), T("f"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_ancestor('btwo', current_setting('test.main1', TRUE)), has_ancestor('btwo', current_setting('test.main2', TRUE)), has_ancestor('btwo', current_setting('test.bone1', TRUE)), has_ancestor('btwo', current_setting('test.bone2', TRUE)), has_ancestor('btwo', current_setting('test.btwo1', TRUE)), has_ancestor('btwo', current_setting('test.onetwo1', TRUE)), has_ancestor('btwo', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("f"), T("f"), T("t"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_ancestor('onetwo', current_setting('test.main1', TRUE)), has_ancestor('onetwo', current_setting('test.main2', TRUE)), has_ancestor('onetwo', current_setting('test.bone1', TRUE)), has_ancestor('onetwo', current_setting('test.bone2', TRUE)), has_ancestor('onetwo', current_setting('test.btwo1', TRUE)), has_ancestor('onetwo', current_setting('test.onetwo1', TRUE)), has_ancestor('onetwo', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_ancestor(commit_hash, 'btwo') FROM dolt_log WHERE commit_hash = current_setting('test.onetwo1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_ancestor(commit_hash, 'btwo') FROM dolt_log AS OF 'onetwo' WHERE commit_hash = current_setting('test.onetwo1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_ancestor('HEAD', 'tag_btwo1'), has_ancestor(current_setting('test.bone2', TRUE), 'tag_btwo1'),has_ancestor(current_setting('test.onetwo1', TRUE), 'tag_btwo1'), has_ancestor(current_setting('test.btwo1', TRUE), 'tag_btwo1'), has_ancestor(current_setting('test.main2', TRUE), 'tag_btwo1'), has_ancestor(current_setting('test.main1', TRUE), 'tag_btwo1');",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL)],
                        rows: &[
                            &[T("f"), T("f"), T("t"), T("t"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_ancestor('tag_btwo1', 'HEAD'), has_ancestor('tag_btwo1', current_setting('test.bone2', TRUE)), has_ancestor('tag_btwo1', current_setting('test.onetwo1', TRUE)), has_ancestor('tag_btwo1', current_setting('test.btwo1', TRUE)), has_ancestor('tag_btwo1', current_setting('test.main2', TRUE)), has_ancestor('tag_btwo1', current_setting('test.main1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL), Column("has_ancestor", BOOL)],
                        rows: &[
                            &[T("f"), T("f"), T("f"), T("t"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('onetwo');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_ancestor(commit_hash, 'btwo') FROM dolt_log WHERE commit_hash = current_setting('test.onetwo1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("has_ancestor", BOOL)],
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
            name: "test null filtering in secondary indexes (https://github.com/dolthub/dolt/issues/4199)",
            intent: "IS NULL and IS NOT NULL filters through a secondary index return the right rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, d timestamp); CREATE INDEX index1 ON t (d);",
                "INSERT INTO t (d) VALUES (NOW()), (NOW());",
                "INSERT INTO t (d) VALUES (NULL), (NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE d IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE d IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE d IS NOT DISTINCT FROM NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE NOT(d IS NOT DISTINCT FROM NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(coalesce(t.d, 1)) FROM t, t AS t2 WHERE t.d IS NOT NULL AND t.pk = t2.pk AND t2.d IS NOT NULL;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "COALESCE types timestamp without time zone and integer cannot be matched", position: 28, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(coalesce(t.d, 1)) FROM t, t AS t2 WHERE t.d IS NULL AND t.pk = t2.pk AND t2.d IS NULL;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "COALESCE types timestamp without time zone and integer cannot be matched", position: 28, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(coalesce(t.d, 1)) FROM t, t AS t2 WHERE t.d IS NULL AND t.pk = t2.pk AND t2.d IS NOT NULL;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "COALESCE types timestamp without time zone and integer cannot be matched", position: 28, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test backticks in index name (https://github.com/dolthub/dolt/issues/3776)",
            intent: "An index whose name needs quoting can be created and used.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX \"\"\"i\"\"\" ON t (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("\"i\"")],
                            &[T("t_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_indexes lists the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test AS OF indexed queries (https://github.com/dolthub/dolt/issues/7488)",
            intent: "AS OF reads through an index return the revision's rows.",
            set_up_script: &[
                "CREATE TABLE indexedTable (pk int PRIMARY KEY, c0 int, c1 varchar(255)); CREATE INDEX c1_idx ON indexedTable (c1);",
                "INSERT INTO indexedTable (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t with index');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "UPDATE indexedTable SET c1='two';",
                "SELECT dolt_commit('-am', 'updating one to two');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT c1 FROM indexedTable;",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM indexedTable WHERE c1 > 'o';",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM indexedTable AS OF current_setting('test.commit2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM indexedTable AS OF current_setting('test.commit2', TRUE) WHERE c1 > 'o';",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM indexedTable AS OF current_setting('test.commit1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM indexedTable AS OF current_setting('test.commit1', TRUE) WHERE c1 > 'o';",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test AS OF vector indexed queries",
            intent: "A vector index read AS OF a commit returns that commit's nearest rows.",
            set_up_script: &[
                "CREATE TABLE vectors (id int PRIMARY KEY, v vector(2) NOT NULL); CREATE INDEX v_idx ON vectors USING hnsw (v vector_l2_ops);",
                "INSERT INTO vectors VALUES (1, '[3.0,4.0]'), (2, '[1.0,1.0]'), (3, '[5.0,5.0]');",
                "SELECT dolt_commit('-Am', 'adding table vectors with vector index');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO vectors VALUES (4, '[0.0,0.0]');",
                "SELECT dolt_commit('-am', 'adding closest vector');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id FROM vectors ORDER BY v <-> '[0.0,0.0]' LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM vectors AS OF current_setting('test.commit1', TRUE) ORDER BY v <-> '[0.0,0.0]' LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "test as of indexed join (https://github.com/dolthub/dolt/issues/2189)",
            intent: "A join of a table AS OF two commits through an index returns each commit's rows.",
            set_up_script: &[
                "CREATE TABLE a (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO a VALUES (1,1), (2,2), (3,3);",
                "SELECT dolt_commit('-a', '-m', 'first commit');",
                "INSERT INTO a VALUES (4,4), (5,5), (6,6);",
                "SELECT dolt_commit('-a', '-m', 'second commit');",
                "SELECT set_config('test.second_commit', ((SELECT commit_hash FROM dolt_log ORDER BY date DESC LIMIT 1))::text, FALSE);",
                "SELECT set_config('test.first_commit', ((SELECT commit_hash FROM dolt_log ORDER BY date DESC LIMIT 1 OFFSET 1))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a1.* FROM a AS OF current_setting('test.second_commit', TRUE) a1 LEFT JOIN a AS OF current_setting('test.first_commit', TRUE) a2 ON a1.pk = a2.pk WHERE a2.pk IS NULL ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a1.* FROM a AS OF current_setting('test.second_commit', TRUE) a1 LEFT JOIN a AS OF current_setting('test.second_commit', TRUE) a2 ON a1.pk = a2.pk WHERE a2.pk IS NULL ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
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
            name: "Prepared ASOF",
            intent: "A prepared AS OF read binds its parameter at each revision.",
            set_up_script: &[
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0,0), (1,1);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table'), FALSE);",
                "SELECT dolt_branch('-c', 'main', 'newb');",
                "ALTER TABLE test ADD COLUMN c2 int;",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'alter table'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~' WHERE pk = $1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    bind_vars: &[BindVar::Int(0)],
                    dolt: Some(Dolt::MySqlOnly("Dolt's ? placeholder is Postgres' $1, bound as a prepared statement's parameter")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF dolt_hashof('HEAD') WHERE pk = $1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    bind_vars: &[BindVar::Int(1)],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF current_setting('test.Commit1', TRUE) WHERE pk = $1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    bind_vars: &[BindVar::Int(0)],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF current_setting('test.Commit2', TRUE) WHERE pk = $1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    bind_vars: &[BindVar::Int(1)],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "blame: composite pk ordered output with correct header (bats repro)",
            intent: "dolt_blame of a table with a two-column key lists each row in key order.",
            set_up_script: &[
                "CREATE TABLE t(pk varchar(20), val int);",
                "ALTER TABLE t ADD PRIMARY KEY (pk, val);",
                "INSERT INTO t VALUES ('zzz',4),('mult',1),('sub',2),('add',5);",
                "SELECT dolt_commit('-Am', 'add rows');",
                "INSERT INTO t VALUES ('dolt',0),('alt',12),('del',8),('ctl',3);",
                "SELECT dolt_commit('-am', 'add more rows');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk, val, dolt_message FROM dolt_blame_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("val", INT4), Column("dolt_message", TEXT)],
                        rows: &[
                            &[T("add"), T("5"), T("add rows")],
                            &[T("alt"), T("12"), T("add more rows")],
                            &[T("ctl"), T("3"), T("add more rows")],
                            &[T("del"), T("8"), T("add more rows")],
                            &[T("dolt"), T("0"), T("add more rows")],
                            &[T("mult"), T("1"), T("add rows")],
                            &[T("sub"), T("2"), T("add rows")],
                            &[T("zzz"), T("4"), T("add rows")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_blame_T;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "blame: table and pk require identifier quoting",
            intent: "dolt_blame of a table and key whose names need quoting works.",
            set_up_script: &[
                "CREATE TABLE \"t-1\" (\"p-k\" int PRIMARY KEY, col1 varchar(100));",
                "INSERT INTO \"t-1\" VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t-1');",
                "INSERT INTO \"t-1\" VALUES (2, 'two');",
                "SELECT dolt_commit('-Am', 'adding another row to t-1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT \"p-k\", dolt_message FROM \"dolt_blame_t-1\";",
                    expected: Expected::Rows {
                        columns: &[Column("p-k", INT4), Column("dolt_message", TEXT)],
                        rows: &[
                            &[T("1"), T("adding table t-1")],
                            &[T("2"), T("adding another row to t-1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_docs panic",
            intent: "An upsert into dolt_docs updates the existing doc.",
            set_up_script: &[
                "INSERT INTO dolt_docs VALUES ('name','content1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_docs VALUES ('name','content2')  ON CONFLICT (\"doc_name\") DO UPDATE SET doc_text = '789';",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres counts an upserted row once, where MySQL counts a row its ON DUPLICATE KEY UPDATE changes twice")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_docs ORDER BY doc_name DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR), Column("doc_text", TEXT)],
                        rows: &[
                            &[T("name"), T("789")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's repositories start with an AGENT.md doc, where a Doltgrust database starts with no docs")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Nautobot FOREIGN KEY panic repro",
            intent: "A row whose foreign key references an existing row of another table inserts.",
            set_up_script: &[
                "CREATE TABLE \"auth_user\" (\"password\" varchar(128) NOT NULL, \"last_login\" timestamp, \"is_superuser\" smallint NOT NULL, \"username\" varchar(150) NOT NULL, \"first_name\" varchar(150) NOT NULL, \"last_name\" varchar(150) NOT NULL, \"email\" varchar(254) NOT NULL, \"is_staff\" smallint NOT NULL, \"is_active\" smallint NOT NULL, \"date_joined\" timestamp NOT NULL, \"id\" char(32) NOT NULL, \"config_data\" json NOT NULL, PRIMARY KEY (\"id\"))   ; CREATE UNIQUE INDEX \"username\" ON \"auth_user\" (\"username\");",
                "CREATE TABLE \"users_token\" (\"id\" char(32) NOT NULL, \"created\" timestamp NOT NULL, \"expires\" timestamp, \"key\" varchar(40) NOT NULL, \"write_enabled\" smallint NOT NULL, \"description\" varchar(200) NOT NULL, \"user_id\" char(32) NOT NULL, PRIMARY KEY (\"id\"), CONSTRAINT \"users_token_user_id_af964690_fk_auth_user_id\" FOREIGN KEY (\"user_id\") REFERENCES \"auth_user\" (\"id\"))   ; CREATE UNIQUE INDEX \"key\" ON \"users_token\" (\"key\"); CREATE INDEX \"users_token_user_id_af964690\" ON \"users_token\" (\"user_id\");",
                "INSERT INTO \"auth_user\" (\"password\",\"last_login\",\"is_superuser\",\"username\",\"first_name\",\"last_name\",\"email\",\"is_staff\",\"is_active\",\"date_joined\",\"id\",\"config_data\")VALUES ('pbkdf2_sha256$216000$KRpZeDPgwc5E$vl/2hwrmtnckaBT0A8pf63Ph+oYuCHYI7qozMTZihTo=',NULL,1,'admin','','','admin@example.com',1,1,'2022-08-30 18:27:21.810049','1056443cc03446c592fa4c06bb06a1a6','{}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"users_token\" (\"id\", \"user_id\", \"created\", \"expires\", \"key\", \"write_enabled\", \"description\") VALUES ('acc2e157db2845a79221cc654b1dcecc', '1056443cc03446c592fa4c06bb06a1a6', '2022-08-30 18:27:21.948487', NULL, '0123456789abcdef0123456789abcdef01234567', 1, '');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "descending index columns",
            intent: "A descending index column keeps its order through diffs, patches, and type changes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b int NOT NULL); CREATE INDEX ab ON t (a DESC, b);",
                "INSERT INTO t VALUES (1, 1, 1), (2, 1, 2), (3, 2, 1), (4, NULL, 3), (5, 3, 4);",
                "SELECT dolt_commit('-Am', 'table with a descending index');",
                "CREATE INDEX b_desc ON t (b DESC);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~', 'HEAD', 't') WHERE diff_type = 'schema';",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    a integer,\n    b integer NOT NULL\n);")],
                            &[T("ALTER TABLE public.t OWNER TO postgres;")],
                            &[T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);")],
                            &[T("CREATE INDEX ab ON public.t USING btree (a DESC, b);")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE INDEX b_desc ON public.t USING btree (b DESC);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("ab"), T("CREATE INDEX ab ON public.t USING btree (a DESC, b)")],
                            &[T("b_desc"), T("CREATE INDEX b_desc ON public.t USING btree (b DESC)")],
                            &[T("t_pkey"), T("CREATE UNIQUE INDEX t_pkey ON public.t USING btree (pk)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE and information_schema.STATISTICS are MySQL's, so pg_indexes shows each index's columns and their order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t WHERE a > 0 ORDER BY a DESC, b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: a DESC, b")],
                            &[T("  ->  Seq Scan on t")],
                            &[T("        Filter: (a > 0)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t WHERE a > 0 ORDER BY a, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: a, b DESC")],
                            &[T("  ->  Seq Scan on t")],
                            &[T("        Filter: (a > 0)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t WHERE a > 0 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: a, b")],
                            &[T("  ->  Seq Scan on t")],
                            &[T("        Filter: (a > 0)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t ORDER BY b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: b DESC")],
                            &[T("  ->  Seq Scan on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE a > 0 ORDER BY a DESC, b;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("3")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE a > 0 ORDER BY a, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("1")],
                            &[T("3")],
                            &[T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t ORDER BY b DESC, pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("4")],
                            &[T("2")],
                            &[T("1")],
                            &[T("3")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE a IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE a = 1 AND b > 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a DROP DEFAULT, ALTER COLUMN a TYPE bigint USING a::bigint, ALTER COLUMN a DROP NOT NULL;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE a > 1 ORDER BY a DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 't') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t ALTER COLUMN a TYPE bigint;")],
                            &[T("CREATE INDEX b_desc ON public.t USING btree (b DESC);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "descending index chosen among candidates",
            intent: "Queries ordered by the columns of descending indexes return rows in the asked order.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b int); CREATE INDEX ab ON t (a DESC, b); CREATE INDEX ab_desc ON t (a DESC, b DESC);",
                "INSERT INTO t VALUES (1, 1, 1), (2, 1, 2), (3, 2, 1), (4, NULL, 3), (5, 2, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t ORDER BY a DESC, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using ab_desc on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan using ab_desc on t"])),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan Backward using ab_desc on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan Backward using ab_desc on t"])),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t ORDER BY a DESC, b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using ab on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan using ab on t"])),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t ORDER BY a DESC, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                            &[T("3")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("5")],
                            &[T("4")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t ORDER BY a DESC, b;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("3")],
                            &[T("5")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "descending indexes across branches and merges",
            intent: "Descending indexes made on a branch merge and keep their order.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b VARCHAR(10)); CREATE INDEX idx_a ON t (a DESC);",
                "INSERT INTO t VALUES (1, 3, 'c'), (2, 1, 'a'), (3, NULL, 'b');",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "CREATE INDEX idx_ab ON t (a, b DESC);",
                "INSERT INTO t VALUES (4, 2, 'd');",
                "SELECT dolt_commit('-Am', 'add index on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 4, 'e');",
                "SELECT dolt_commit('-Am', 'insert on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    a integer,\n    b character varying(10)\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);\n\nCREATE INDEX idx_a ON public.t USING btree (a DESC);"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    a integer,\n    b character varying(10)\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);\n\nCREATE INDEX idx_a ON public.t USING btree (a DESC);\n\nCREATE INDEX idx_ab ON public.t USING btree (a, b DESC);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('main', 'branch1', 't') WHERE diff_type = 'schema';",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE INDEX idx_ab ON public.t USING btree (a, b DESC);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t AS OF 'branch1' ORDER BY a DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("1")],
                            &[T("4")],
                            &[T("2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_a"), T("CREATE INDEX idx_a ON public.t USING btree (a DESC)")],
                            &[T("idx_ab"), T("CREATE INDEX idx_ab ON public.t USING btree (a, b DESC)")],
                            &[T("t_pkey"), T("CREATE UNIQUE INDEX t_pkey ON public.t USING btree (pk)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE and information_schema.STATISTICS are MySQL's, so pg_indexes shows each index's columns and their order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, dolt_diff_type FROM dolt_diff('HEAD~', 'HEAD', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t ORDER BY a DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("5")],
                            &[T("1")],
                            &[T("4")],
                            &[T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, a, b FROM t ORDER BY a, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", VARCHAR)],
                        rows: &[
                            &[T("2"), T("1"), T("a")],
                            &[T("4"), T("2"), T("d")],
                            &[T("1"), T("3"), T("c")],
                            &[T("5"), T("4"), T("e")],
                            &[T("3"), Null, T("b")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk, a, b FROM t ORDER BY a, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: a, b DESC")],
                            &[T("  ->  Seq Scan on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM t ORDER BY a DESC LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: a DESC")],
                            &[T("        ->  Seq Scan on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t ORDER BY a DESC LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_ab;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DROP INDEX public.idx_ab;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_a"), T("CREATE INDEX idx_a ON public.t USING btree (a DESC)")],
                            &[T("idx_ab"), T("CREATE INDEX idx_ab ON public.t USING btree (a, b DESC)")],
                            &[T("t_pkey"), T("CREATE UNIQUE INDEX t_pkey ON public.t USING btree (pk)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE and information_schema.STATISTICS are MySQL's, so pg_indexes shows each index's columns and their order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE a = 2 AND b < 'z';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            name: "descending index scans over several ranges",
            intent: "An IN filter on a descending index returns rows in the asked order.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b int); CREATE INDEX a_desc ON t (a DESC); CREATE INDEX ab ON t (a, b DESC);",
                "INSERT INTO t VALUES (1, 10, 1), (2, 20, 2), (3, 30, 3), (4, 40, 4), (5, NULL, 5), (6, 20, 6), (7, 30, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM t WHERE a IN (10, 30, 40) ORDER BY a DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: a DESC")],
                            &[T("  ->  Bitmap Heap Scan on t")],
                            &[T("        Recheck Cond: (a = ANY ('{10,30,40}'::integer[]))")],
                            &[T("        ->  Bitmap Index Scan on ab")],
                            &[T("              Index Cond: (a = ANY ('{10,30,40}'::integer[]))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows its own plan")),
                    skip: Some("Doltgrust's index-only scans read no table pages, so it counts every page all-visible and plans an Index Only Scan where PG18 sorts a bitmap scan, Appendix C \"all-visible pages before the first VACUUM\""),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE a IN (10, 30, 40) ORDER BY a DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("40")],
                            &[T("30")],
                            &[T("30")],
                            &[T("10")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE a IN (10, 30, 40) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("30")],
                            &[T("30")],
                            &[T("40")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE a < 20 OR a > 30 ORDER BY a DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("40")],
                            &[T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE a < 20 OR a > 30 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("40")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE a > 30 OR b = 2 ORDER BY a DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("40")],
                            &[T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE a > 30 OR b = 2 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("20")],
                            &[T("40")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t WHERE a IN (20, 30) ORDER BY a DESC, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("30"), T("3")],
                            &[T("30"), Null],
                            &[T("20"), T("2")],
                            &[T("20"), T("6")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t WHERE a IN (20, 30) ORDER BY a, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("20"), T("6")],
                            &[T("20"), T("2")],
                            &[T("30"), Null],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Query table with 10K rows ",
            intent: "A table of 10,000 rows counts and pages correctly.",
            set_up_script: &[
                "CREATE TABLE bigTable (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO bigTable SELECT i, i FROM generate_series(0, 9999) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bigTable;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM bigTable ORDER BY pk LIMIT 5 OFFSET 9990;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("9990"), T("9990")],
                            &[T("9991"), T("9991")],
                            &[T("9992"), T("9992")],
                            &[T("9993"), T("9993")],
                            &[T("9994"), T("9994")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sequence values as changes of their tables or of themselves",
            intent: "A value change of a table's own sequence is a schema change of the table that stages and commits, a standalone sequence's is its own, and a value set back to the committed one is no change.",
            set_up_script: &[
                "CREATE TABLE ainc (id int NOT NULL GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, v int);",
                "CREATE TABLE ser (id serial PRIMARY KEY);",
                "CREATE SEQUENCE s;",
                "INSERT INTO ainc (v) VALUES (1);",
                "SELECT dolt_commit('-Am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT nextval(pg_get_serial_sequence('ainc', 'id'));",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.ainc"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_table_name, data_change, schema_change FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.ainc"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'nextval');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ser DEFAULT VALUES;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ser;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.ser"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('ser');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.ser"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'ser');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT nextval('s');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("sequence public.s"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 's');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setval(pg_get_serial_sequence('ser', 'id'), 1, true);",
                    expected: Expected::Rows {
                        columns: &[Column("setval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
            name: "revisions that ask for an ancestor past the first commit",
            intent: "Every function that reads a revision refuses an ancestor its commit lacks with Dolt's invalid ancestor spec, and a diff names the branch a missing revision starts from.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'one');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('main~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD^3');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD~5';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main..HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD~5', 'HEAD', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD~5..HEAD', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD...HEAD~5', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~5', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~5', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD~5', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('HEAD~5', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_column_diff AS OF 'HEAD~5';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('b', 'HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: Unexpected error creating branch 'b' : invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'b2', 'HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: Unexpected error creating branch 'b2' : invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('HEAD~5', '--', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--soft', 'HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1', 'HEAD~5');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('nosuch~1', 'HEAD', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: nosuch", ..E }),
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'main^0';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust takes a revision ending in `^0` as the commit itself, as Git does, where Dolt refuses it, by the owner's decision 2026-10-27 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD^2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_commit_tests() {
    run_scripts(&[
        ScriptTest {
            name: "CALL DOLT_COMMIT('-ALL') adds all tables (including new ones) to the commit.",
            intent: "dolt_commit with -ALL stages and commits every change, a dropped table and a new one included, and --amend replaces the last commit's message and contents.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_add('t');",
                "SELECT dolt_commit('-m', 'add table t');",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-ALL', '-m', 'update table terminator');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("update table terminator")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-amend', '-m', 'update table t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("update table t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'drop table t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'add table 21');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-amend', '-m', 'add table 2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("add table 2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the expectation leaves out the error's position 15"),
            ..S
        },
        ScriptTest {
            name: "dolt commit works with arguments",
            intent: "dolt_commit takes its message from an expression, which dolt_log then shows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_add('t');",
                "SELECT dolt_commit('-m', concat('author: ', 'somebody'));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log WHERE message = 'author: somebody';",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("author: somebody")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_COMMIT('-amend') works to update commit message",
            intent: "dolt_commit --amend with a message replaces the last commit's message and keeps its changes.",
            set_up_script: &[
                "CREATE TABLE test (id INT PRIMARY KEY);",
                "INSERT INTO test (id) VALUES (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original commit message');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("original commit message")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Setup("Dolt's shared harness keeps the five commits of this suite's earlier scripts and its checkpoint commit; ours shows only this script's commit and the initial one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_id, from_id, dolt_diff_type FROM dolt_diff_test;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("from_id", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'amended commit message');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
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
                            &[T("amended commit message")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Setup("Dolt's shared harness keeps the five commits of this suite's earlier scripts and its checkpoint commit; ours shows only this script's commit and the initial one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_id, from_id, dolt_diff_type FROM dolt_diff_test;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("from_id", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), Null, T("added")],
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
            name: "CALL DOLT_COMMIT('--amend') works on initial commit",
            intent: "dolt_commit --amend on a branch at the initial commit replaces that commit, leaving one commit.",
            set_up_script: &[
                "SELECT set_config('test.hash', (SELECT commit_hash FROM dolt_log ORDER BY commit_order ASC LIMIT 1), false);",
                "SELECT dolt_branch('initcommit', current_setting('test.hash'));",
                "SELECT dolt_checkout('initcommit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT dolt_commit('--amend', '-m', 'amended commit message');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
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
                            &[T("amended commit message")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT dolt_commit('--amend', '-m', 'amended commit message x2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
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
                            &[T("amended commit message x2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
            name: "CALL DOLT_COMMIT('-amend') works to add changes to a commit",
            intent: "dolt_commit --amend folds newly staged changes into the last commit, keeping its message unless a new one is given, and leaves nothing to commit.",
            set_up_script: &[
                "CREATE TABLE test (id INT PRIMARY KEY);",
                "INSERT INTO test (id) VALUES (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'amended commit message');",
                "INSERT INTO test (id) VALUES (3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original commit message for adding changes to a commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_id, from_id, dolt_diff_type FROM dolt_diff_test;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("from_id", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), Null, T("added")],
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("original commit message for adding changes to a commit")],
                            &[T("amended commit message")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's shared harness carries the table test and the commits of this suite's earlier scripts with its checkpoint commit; ours makes the table and its one earlier commit in the setup and shows those and the initial commit")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (id) VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
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
                            &[T("original commit message for adding changes to a commit")],
                            &[T("amended commit message")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's shared harness carries the table test and the commits of this suite's earlier scripts with its checkpoint commit; ours makes the table and its one earlier commit in the setup and shows those and the initial commit")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_id, from_id, dolt_diff_type FROM dolt_diff_test;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("from_id", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), Null, T("added")],
                            &[T("3"), Null, T("added")],
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (id) VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'amended commit with added changes');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("amended commit with added changes")],
                            &[T("amended commit message")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's shared harness carries the table test and the commits of this suite's earlier scripts with its checkpoint commit; ours makes the table and its one earlier commit in the setup and shows those and the initial commit")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_id, from_id, dolt_diff_type FROM dolt_diff_test;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("from_id", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("5"), Null, T("added")],
                            &[T("4"), Null, T("added")],
                            &[T("3"), Null, T("added")],
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_COMMIT('-amend') works to remove changes from a commit",
            intent: "dolt_commit --amend with a staged delete removes that row from the last commit.",
            set_up_script: &[
                "CREATE TABLE test (id INT PRIMARY KEY);",
                "INSERT INTO test (id) VALUES (2), (3), (4), (5);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'amended commit with added changes');",
                "INSERT INTO test (id) VALUES (6);",
                "INSERT INTO test (id) VALUES (7);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original commit message');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_id, from_id, dolt_diff_type FROM dolt_diff_test;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("from_id", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("7"), Null, T("added")],
                            &[T("6"), Null, T("added")],
                            &[T("5"), Null, T("added")],
                            &[T("4"), Null, T("added")],
                            &[T("3"), Null, T("added")],
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE id = 6;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'amended commit with removed changes');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("7")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("amended commit with removed changes")],
                            &[T("amended commit with added changes")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's shared harness carries the table test and the commits of this suite's earlier scripts with its checkpoint commit; ours makes the table and its one earlier commit in the setup and shows those and the initial commit")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_id, from_id, dolt_diff_type FROM dolt_diff_test;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("from_id", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("7"), Null, T("added")],
                            &[T("5"), Null, T("added")],
                            &[T("4"), Null, T("added")],
                            &[T("3"), Null, T("added")],
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_COMMIT('-amend') works to update a merge commit",
            intent: "dolt_commit --amend of a merge commit keeps both of its parents.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE test2 (id INT PRIMARY KEY, id2 INT);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original table');",
                "SELECT dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test2 (id, id2) VALUES (0, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'conflicting commit message');",
                "SELECT dolt_checkout('main');",
                "INSERT INTO test2 (id, id2) VALUES (0, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original commit message');",
                "SELECT dolt_merge('test-branch');",
                "SELECT dolt_conflicts_resolve('--theirs', '.');",
                "SELECT dolt_commit('-m', 'final merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'new merge');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
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
                            &[T("new merge")],
                            &[T("original commit message")],
                            &[T("conflicting commit message")],
                            &[T("original table")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Setup("Dolt's shared harness keeps the commits of this suite's earlier scripts and its checkpoint commit; ours shows only this script's commits and the initial one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.hash', (SELECT commit_hash FROM dolt_log LIMIT 1), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(parent_hash) FROM dolt_commit_ancestors WHERE commit_hash = current_setting('test.hash');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "dolt_commits committer columns when unset reflect --author",
            intent: "When no committer is set, dolt_commit's --author also gives the commit's committer.",
            set_up_script: &[
                "CREATE TABLE commits_author_tbl (pk INT PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'author commit', '--author', 'Test Author <author@example.com>');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT committer, email, author, author_email FROM dolt_commits WHERE message = 'author commit';",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Test Author"), T("author@example.com"), T("Test Author"), T("author@example.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_COMMIT with --amend fails while a merge or cherry-pick is in progress",
            intent: "dolt_commit --amend fails while a merge or a cherry-pick is in progress and leaves its state alone.",
            set_up_script: &[
                "DROP TABLE IF EXISTS invalidFK;",
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE amend_merge_t (pk int PRIMARY KEY, c int);",
                "INSERT INTO amend_merge_t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'other');",
                "UPDATE amend_merge_t SET c = 2 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'other change');",
                "SELECT dolt_checkout('main');",
                "UPDATE amend_merge_t SET c = 3 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'main change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T(r#"("",0,1,"conflicts found")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '--amend', '-m', 'amend during merge');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "you are in the middle of a merge -- cannot amend", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_conflicts_amend_merge_t;",
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
                    query: "SELECT dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T(r#"("",0,0,"merge aborted")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_cherry_pick(dolt_hashof('other'));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_cherry_pick", RECORD)],
                        rows: &[
                            &[T(r#"("",1,0,0)"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '--amend', '-m', 'amend during cherry-pick');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "you are in the middle of a cherry-pick -- cannot amend", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_cherry_pick('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_cherry_pick", RECORD)],
                        rows: &[
                            &[T(r#"("",0,0,0)"#)],
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
fn test_dolt_reset_test_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "CALL DOLT_RESET('--hard') should reset the merge state after uncommitted merge",
            intent: "dolt_reset --hard after a merge that made no commit ends the merge, so there is no merge to abort.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "INSERT INTO test1 VALUES (0,1,1);",
                "SELECT dolt_commit('-am', 'added table');",
                "SELECT dolt_checkout('-b', 'merge_branch');",
                "UPDATE test1 SET c1 = 2;",
                "SELECT dolt_commit('-am', 'update pk 0 = 2,1 to test1');",
                "SELECT dolt_checkout('main');",
                "UPDATE test1 SET c2 = 2;",
                "SELECT dolt_commit('-am', 'update pk 0 = 1,2 to test1');",
                "SELECT dolt_merge('merge_branch');",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('--abort');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "fatal: There is no merge to abort", ..E }),
                    ..A
                },
            ],
            skip: Some("dolt_merge('--abort') without a merge in progress fails with 22023, where the port expects 55000"),
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_RESET('--hard') should reset the merge state after conflicting merge",
            intent: "dolt_reset --hard after a merge with conflicts ends the merge, so there is no merge to abort.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = on;",
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "INSERT INTO test1 VALUES (0,1,1);",
                "SELECT dolt_commit('-am', 'added table');",
                "SELECT dolt_checkout('-b', 'merge_branch');",
                "UPDATE test1 SET c1 = 2, c2 = 2;",
                "SELECT dolt_commit('-am', 'update pk 0 = 2,2 to test1');",
                "SELECT dolt_checkout('main');",
                "UPDATE test1 SET c1 = 3, c2 = 3;",
                "SELECT dolt_commit('-am', 'update pk 0 = 3,3 to test1');",
                "SELECT dolt_merge('merge_branch');",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('--abort');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "fatal: There is no merge to abort", ..E }),
                    ..A
                },
            ],
            skip: Some("dolt_merge('--abort') without a merge in progress fails with 22023, where the port expects 55000"),
            ..S
        },
        ScriptTest {
            name: "dolt_reset('--hard') commits the active SQL transaction",
            intent: "dolt_reset --hard to HEAD~ inside a transaction block leaves nothing changed against the new head.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'creating table t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'HEAD~');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
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
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset('--soft') commits the active SQL transaction",
            intent: "dolt_reset --soft to HEAD~ moves only the head, so the table the staged root still has shows as a new staged table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'creating table t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--soft', 'HEAD~');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
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
                            &[T("public.t"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset() commits the active SQL transaction",
            intent: "dolt_reset to HEAD~ moves the head and the staged root, so the table only the working state has shows as a new unstaged table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'creating table t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('HEAD~');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
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
                            &[T("public.t"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset(rev) moves HEAD, resets the index, and leaves the working tree alone",
            intent: "dolt_reset to a revision makes it the head and the staged root and leaves the working state's rows as they are.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3, 30);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (4, 40);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('HEAD~');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, v FROM t AS OF STAGED ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, v FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset('--soft', rev) leaves staged tables untouched",
            intent: "dolt_reset --soft to a revision moves only the head, so the staged rows show as a staged change against it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3, 30);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--soft', 'HEAD~');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t AS OF STAGED ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
                    query: "SELECT pk FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset('--hard') preserves ignored tables when ignore rule is committed",
            intent: "dolt_reset --hard leaves a table dolt_ignore matches as it is and resets the tracked ones.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('private_data', true);",
                "SELECT dolt_commit('-Am', 'add ignore rule');",
                "CREATE TABLE mytable (pk int PRIMARY KEY, val int);",
                "INSERT INTO mytable VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'add mytable');",
                "CREATE TABLE private_data (pk int PRIMARY KEY, secret varchar(100));",
                "INSERT INTO private_data VALUES (1, 'secret');",
                "INSERT INTO mytable VALUES (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, secret FROM private_data;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("secret", VARCHAR)],
                        rows: &[
                            &[T("1"), T("secret")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
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
    ]);
}

#[test]
fn test_dolt_branch_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "Create branches from HEAD with dolt_branch procedure",
            intent: "dolt_branch makes branches at HEAD, refuses a name that is taken, invalid, or empty, and refuses usage it doesn't take.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('myNewBranch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_BRANCHES WHERE NAME='myNewBranch1';",
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
                    query: "SELECT dolt_branch('myNewBranch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'myNewBranch1' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', 'myNewBranch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
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
            name: "Create branches from HEAD fails when using a non-branch revision",
            intent: "dolt_branch fails in a detached head, a revision database at a commit, and makes, renames, and force-renames branches once the session is on a branch again.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'checkpoint');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('myNewBranch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main~",
                    database: "postgres/main~",
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: dolt_branch on a commit's session makes the branch at that commit, where Dolt refuses it in a detached head")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Rename branches with dolt_branch procedure",
            intent: "dolt_branch -m renames a branch, keeping its commits, and refuses a missing branch, a taken name, and empty names.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('myNewBranch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('myNewBranch2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'myNewBranch1', 'myNewBranch2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'myNewBranch2' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-mf', 'myNewBranch1', 'myNewBranch2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'myNewBranch2', 'myNewBranch3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'myNewBranch3', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "not a valid user branch name", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Copy branches from other branches using dolt_branch procedure",
            intent: "dolt_branch -c copies a branch to a new name at the same head, -f overwrites the target, and a missing source or taken name is refused.",
            set_up_script: &[
                "SELECT dolt_branch('myNewBranch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: invalid usage", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'myNewBranch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: invalid usage", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'myNewBranch2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: invalid usage", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', '', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: cannot branch empty string", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'myNewBranch1', 'myNewBranch2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_BRANCHES WHERE NAME='myNewBranch2';",
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
                    query: "SELECT dolt_branch('-c', 'myNewBranch1', 'myNewBranch2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'myNewBranch2' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-cf', 'myNewBranch1', 'myNewBranch2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'myNewBranch1', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: 'HEAD' is not a valid branch name.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Delete branches with dolt_branch procedure",
            intent: "dolt_branch -d deletes a branch merged into HEAD and refuses an unmerged one, -D deletes either, and the checked-out branch can't be deleted.",
            set_up_script: &[
                "SELECT dolt_branch('myNewBranch1');",
                "SELECT dolt_branch('myNewBranch2');",
                "SELECT dolt_branch('myNewBranch3');",
                "SELECT dolt_branch('myNewBranchWithCommit');",
                "SELECT * FROM dolt_checkout('myNewBranchWithCommit');",
                "SELECT dolt_commit('--allow-empty', '-am', 'empty commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: invalid usage", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: cannot branch empty string", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'branchDoesNotExist');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'myNewBranch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_BRANCHES WHERE NAME='myNewBranch1';",
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
                    query: "SELECT dolt_branch('-d', 'myNewBranch2', 'myNewBranch3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'myNewBranchWithCommit');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch 'myNewBranchWithCommit' is not fully merged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-df', 'myNewBranchWithCommit');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
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
            name: "Create branch from startpoint",
            intent: "dolt_checkout -b makes a branch at a start point, HEAD~1 or a commit's hash, and switches to it, refusing a start point that is no commit.",
            set_up_script: &[
                "CREATE TABLE a (x int);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.commit1', ('')::text, FALSE);",
                "SELECT set_config('test.commit1', dolt_commit('-am', 'add table a'), false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newBranch', 'head~1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'newBranch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newBranch2', current_setting('test.commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'newBranch2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'otherBranch', 'unknownCommit');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: 'unknownCommit' is not a commit and a branch 'otherBranch' cannot be created from it", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "-- allows escaping arg parsing to create/delete branch names that look like flags",
            intent: "-- ends the options, so a branch named like a flag can be made and deleted.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name='-b';",
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
                    query: "SELECT dolt_branch('--', '-b');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name='-b';",
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
                    query: "SELECT dolt_branch('-d', '-f', '--', '-b');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name='-b';",
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
            name: "Join same table at two commits",
            intent: "A table read at two revisions in one query joins the rows each revision holds.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_add('t');",
                "SELECT dolt_commit('-m', 'add t');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_add('t');",
                "SELECT dolt_commit('-m', 'insert into t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t CROSS JOIN t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("i", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t CROSS JOIN \"postgres/main\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("i", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a branch name differing from an existing one only by case is rejected",
            intent: "A branch name that differs from an existing one only by case is refused by creation, copying, and renaming, forced or not.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT dolt_branch('br');",
                "SELECT dolt_branch('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('BR');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'br' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', 'BR');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'br' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'BR');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'br' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'br', 'BR');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'br' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'feature', 'BR');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'br' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-cf', 'feature', 'BR');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'br' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-mf', 'feature', 'BR');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'br' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'br' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name = 'br' OR name = 'BR';",
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
            name: "the case collision guard allows same-case reset and case-only rename",
            intent: "The case guard still lets a branch be made again under its own name and renamed to a name differing only by case.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT dolt_branch('br');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'second');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', 'br');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM \"postgres/br\".public.t ORDER by a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'br', 'brcopy');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'br', 'BR');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name = 'br';",
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
                    query: "SELECT count(*) FROM dolt_branches WHERE name = 'BR';",
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
            name: "renaming a branch to its own name preserves the branch and its data",
            intent: "Renaming a branch to its own name keeps the branch and its rows, and a missing branch is refused.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT dolt_branch('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'feature', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-mf', 'feature', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'nosuch', 'nosuch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'feature', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name = 'feature';",
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
                    query: "SELECT a FROM t;",
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
fn test_dolt_checkout_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_checkout changes working set",
            intent: "dolt_checkout switches the session to a branch, whose working set it then reads and writes, and back.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'added values on main');",
                "SELECT * FROM dolt_checkout('b2');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'added values on b2');",
                "SELECT * FROM dolt_checkout('b3');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'added values on b3');",
                "SELECT * FROM dolt_checkout('main');",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout with new branch",
            intent: "dolt_checkout -b makes a branch from the current head and switches to it, and the new branch keeps its own commits.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT * FROM dolt_checkout('-b', 'b2');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'added values on b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout with new branch forcefully",
            intent: "dolt_checkout -B moves an existing branch to a start point, dropping its commits after it, and switches to it.",
            set_up_script: &[
                "CREATE TABLE t (s varchar(5) PRIMARY KEY);",
                "INSERT INTO t VALUES ('foo');",
                "SELECT dolt_commit('-Am', 'commit main~2');",
                "INSERT INTO t VALUES ('bar');",
                "SELECT dolt_commit('-Am', 'commit main~1');",
                "INSERT INTO t VALUES ('baz');",
                "SELECT dolt_commit('-Am', 'commit main');",
                "SELECT dolt_branch('testbr', 'main~1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-B', 'testbr', 'main~2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("testbr")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by s;",
                    expected: Expected::Rows {
                        columns: &[Column("s", VARCHAR)],
                        rows: &[
                            &[T("foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by s;",
                    expected: Expected::Rows {
                        columns: &[Column("s", VARCHAR)],
                        rows: &[
                            &[T("bar")],
                            &[T("baz")],
                            &[T("foo")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-B', 'testbr', 'main~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("testbr")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by s;",
                    expected: Expected::Rows {
                        columns: &[Column("s", VARCHAR)],
                        rows: &[
                            &[T("bar")],
                            &[T("foo")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout with new branch forcefully with dirty working set",
            intent: "dolt_checkout -B resets a branch with uncommitted changes to the start point and switches to it.",
            set_up_script: &[
                "CREATE TABLE t (s varchar(5) PRIMARY KEY);",
                "INSERT INTO t VALUES ('foo');",
                "SELECT dolt_commit('-Am', 'commit main~2');",
                "INSERT INTO t VALUES ('bar');",
                "SELECT dolt_commit('-Am', 'commit main~1');",
                "INSERT INTO t VALUES ('baz');",
                "SELECT dolt_commit('-Am', 'commit main');",
                "SELECT * FROM dolt_checkout('-b', 'testbr', 'main~1');",
                "INSERT INTO t VALUES ('qux');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("testbr")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by s;",
                    expected: Expected::Rows {
                        columns: &[Column("s", VARCHAR)],
                        rows: &[
                            &[T("bar")],
                            &[T("foo")],
                            &[T("qux")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by s;",
                    expected: Expected::Rows {
                        columns: &[Column("s", VARCHAR)],
                        rows: &[
                            &[T("bar")],
                            &[T("baz")],
                            &[T("foo")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-B', 'testbr', 'main~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("testbr")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by s;",
                    expected: Expected::Rows {
                        columns: &[Column("s", VARCHAR)],
                        rows: &[
                            &[T("bar")],
                            &[T("foo")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout mixed with USE statements",
            intent: "dolt_checkout and USE of revision databases both move the session, and active_branch follows the last one.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'added values on main');",
                "SELECT * FROM dolt_checkout('b2');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'added values on b2');",
                "SELECT * FROM dolt_checkout('b3');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'added values on b3');",
                "SELECT * FROM dolt_checkout('main');",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b2",
                    database: "postgres/b2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b2",
                    database: "postgres/b2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
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
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    database: "postgres/main",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout and base name resolution",
            intent: "Unqualified tables resolve in the checked-out branch, qualified ones in the revision database they name.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'added values on main');",
                "SELECT * FROM dolt_checkout('b2');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'added values on b2');",
                "SELECT * FROM dolt_checkout('b3');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'added values on b3');",
                "SELECT * FROM dolt_checkout('main');",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b2",
                    database: "postgres/b2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b2",
                    database: "postgres/b2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM postgres.public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b2",
                    database: "postgres/b2",
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a revision is no database of its own, so the connection's database name reads the connection's branch, where Dolt's base database name reads the base database's checked-out branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM postgres.public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a revision is no database of its own, so the connection's database name reads the connection's branch, where Dolt's base database name reads the base database's checked-out branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
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
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM postgres.public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    database: "postgres/main",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b3",
                    database: "postgres/b3",
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a revision is no database of its own, so the connection's database name reads the connection's branch, where Dolt's base database name reads the base database's checked-out branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout and base name resolution for commit",
            intent: "USE of the base revision database after dolt_checkout moves the session to that branch, where its writes then go.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'added values on main');",
                "SELECT * FROM dolt_checkout('b2');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'added values on b2');",
                "SELECT * FROM dolt_checkout('b3');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'added values on b3');",
                "SELECT * FROM dolt_checkout('b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
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
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch last checked out is deleted",
            intent: "Once the branch the session last checked out is deleted, the session works on the default branch, and revision databases still read each branch's rows.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'added values on main');",
                "SELECT * FROM dolt_checkout('b2');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'added values on b2');",
                "SELECT * FROM dolt_checkout('b3');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'added values on b3');",
                "SELECT * FROM dolt_checkout('b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-df', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    database: "postgres/main",
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
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "main",
                    database: "postgres/main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER by 1;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/b2\" does not exist", ..E }),
                    client: "main",
                    database: "postgres/main",
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a qualifier naming no revision raises Postgres' 3D000 text for a missing database, where Dolt says \"database not found\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Using non-existent refs",
            intent: "USE of a branch, tag, or commit that doesn't exist fails, and the session stays where it was.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_tag('tag1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::ClientError("FATAL: database \"postgres/b2\" does not exist (SQLSTATE 3D000)"),
                    client: "b2",
                    database: "postgres/b2",
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a connection naming no revision fails with Postgres' FATAL 3D000 for a missing database, where Dolt says \"database not found\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "tag1",
                    database: "postgres/tag1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::ClientError("FATAL: database \"postgres/tag2\" does not exist (SQLSTATE 3D000)"),
                    client: "tag2",
                    database: "postgres/tag2",
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a connection naming no revision fails with Postgres' FATAL 3D000 for a missing database, where Dolt says \"database not found\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::ClientError("FATAL: database \"postgres/h4jks5lomp9u41r6902knn0pfr7lsgth\" does not exist (SQLSTATE 3D000)"),
                    client: "h4jks5lomp9u41r6902knn0pfr7lsgth",
                    database: "postgres/h4jks5lomp9u41r6902knn0pfr7lsgth",
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a connection naming no revision fails with Postgres' FATAL 3D000 for a missing database, where Dolt says \"database not found\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/b2\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a qualifier naming no revision raises Postgres' 3D000 text for a missing database, where Dolt says \"database not found\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/tag2\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/tag2\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a qualifier naming no revision raises Postgres' 3D000 text for a missing database, where Dolt says \"database not found\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/h4jks5lomp9u41r6902knn0pfr7lsgth\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/h4jks5lomp9u41r6902knn0pfr7lsgth\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Appendix C, revision connections: a qualifier naming no revision raises Postgres' 3D000 text for a missing database, where Dolt says \"database not found\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Checkout tables from commit",
            intent: "dolt_checkout of tables from a tag or commit gives them that revision's rows and stages nothing else.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "CREATE TABLE t2 (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating tables');",
                "SELECT dolt_tag('tag1');",
                "INSERT INTO t1 VALUES (1, 1);",
                "INSERT INTO t2 VALUES (2, 2);",
                "SELECT dolt_commit('-Am', 'one row in each table');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (3, 3);",
                "INSERT INTO t2 VALUES (4, 4);",
                "SELECT dolt_commit('-Am', 'two rows in each table');",
                "INSERT INTO t1 VALUES (5, 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('HEAD~', '--', 't1');",
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
                    query: "SELECT * FROM t1 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('HEAD~', '--', 't2');",
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
                    query: "SELECT * FROM t1 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
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
                            &[T("public.t2"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1', 't2', 't1');",
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
                    query: "SELECT * FROM t1 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('tag1', '.');",
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
                    query: "SELECT * FROM t1 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("t"), T("modified")],
                            &[T("public.t2"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.commit1', ((SELECT commit_hash FROM dolt_log ORDER by date DESC LIMIT 1))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout(current_setting('test.commit1'), 't1');",
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
                    query: "SELECT * FROM t1 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('nosuchbranch', 't1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: nosuchbranch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('HEAD', 't3');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "tablespec 't3' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout with tracking branch and table with same name",
            intent: "dolt_checkout of a name that is both a remote-tracking branch and a table needs -- to choose the table.",
            set_up_script: &[
                "SELECT dolt_remote('add','origin','file://../remote-repo-483');",
                "CREATE TABLE feature (id int PRIMARY KEY, value int);",
                "INSERT INTO feature VALUES (1, 100);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add feature table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO feature VALUES (2, 200);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add row to feature table');",
                "SELECT dolt_push('origin', 'feature');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE feature SET value = 101 WHERE id = 1;",
                "SELECT dolt_branch('-D', 'feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--', 'feature');",
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
                    query: "SELECT * FROM feature ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "'feature' could be both a local table and a tracking branch.\nPlease use -- to disambiguate.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature', '--');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature'\nbranch 'feature' set up to track 'origin/feature'.")],
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
                            &[T("feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM feature ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout multiple tables with one missing",
            intent: "dolt_checkout of tables fails, naming each missing one, when any table it names doesn't exist.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'created table');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'add row');",
                "INSERT INTO t1 VALUES (2,2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('t1', 'missing');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: tablespec 'missing' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("a statement that fails changes nothing in Postgres, so dolt_checkout restores none of the tables it names when one is missing, where Dolt restores the others (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('t1', 'missing', 'missing2');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: tablespec 'missing' did not match any table(s) known to dolt\nerror: tablespec 'missing2' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--', 'missing', 't1', 'missing2');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: tablespec 'missing' did not match any table(s) known to dolt\nerror: tablespec 'missing2' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout --no-overwrite-ignore aborts when ignored table would be overwritten",
            intent: "dolt_checkout --no-overwrite-ignore refuses to switch when an ignored table would be overwritten.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table and ignore pattern on main', '--force');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--no-overwrite-ignore', 'other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "The following ignored tables would be overwritten by checkout:
	ignored_tbl
Please move or remove them before you switch branches.
Use --overwrite-ignore to force.
", ..E }),
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
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout --overwrite-ignore allows checkout when ignored table would be overwritten",
            intent: "dolt_checkout --overwrite-ignore switches even when an ignored table would be overwritten.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table and ignore pattern on main', '--force');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--overwrite-ignore', 'other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout -b with --no-overwrite-ignore aborts when ignored table differs at start point",
            intent: "dolt_checkout -b --no-overwrite-ignore refuses when an ignored table differs at the start point.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on main', '--force');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table', '--force');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newbranch', '--no-overwrite-ignore', 'HEAD~1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "The following ignored tables would be overwritten by checkout:
	ignored_tbl
Please move or remove them before you switch branches.
Use --overwrite-ignore to force.
", ..E }),
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
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout -b with --overwrite-ignore succeeds when ignored table differs at start point",
            intent: "dolt_checkout -b --overwrite-ignore switches when an ignored table differs at the start point.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on main', '--force');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table', '--force');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newbranch', '--overwrite-ignore', 'HEAD~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("newbranch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout default behavior overwrites ignored tables",
            intent: "dolt_checkout overwrites ignored tables by default when it switches branches.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table and ignore pattern on main', '--force');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout('.') preserves untracked tables",
            intent: "dolt_checkout('.') gives tracked tables HEAD's rows and leaves untracked tables alone.",
            set_up_script: &[
                "CREATE TABLE tracked (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'add tracked');",
                "INSERT INTO tracked VALUES (1);",
                "CREATE TABLE untracked (pk int PRIMARY KEY);",
                "INSERT INTO untracked VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('.');",
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
                    query: "SELECT * FROM tracked;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM untracked;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout('.') preserves ignored tables",
            intent: "dolt_checkout('.') leaves ignored tables alone.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('private_*', TRUE);",
                "SELECT dolt_commit('-Am', 'commit ignore rule');",
                "CREATE TABLE mytable (pk int PRIMARY KEY, val int);",
                "INSERT INTO mytable VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'add mytable');",
                "CREATE TABLE private_data (pk int PRIMARY KEY, secret varchar(100));",
                "INSERT INTO private_data VALUES (1, 'secret');",
                "INSERT INTO mytable VALUES (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('.');",
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
                    query: "SELECT pk, secret FROM private_data;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("secret", VARCHAR)],
                        rows: &[
                            &[T("1"), T("secret")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
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
            name: "dolt_checkout('HEAD', table) does not stage other ignored tables",
            intent: "dolt_checkout('HEAD', table) restores the table without staging any ignored table.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('private_*', TRUE);",
                "SELECT dolt_commit('-Am', 'commit ignore rule');",
                "CREATE TABLE mytable (pk int PRIMARY KEY, val int);",
                "INSERT INTO mytable VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'add mytable');",
                "CREATE TABLE private_data (pk int PRIMARY KEY, secret varchar(100));",
                "INSERT INTO private_data VALUES (1, 'secret');",
                "UPDATE mytable SET val = 99 WHERE pk = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('HEAD', 'mytable');",
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
                    query: "SELECT * FROM mytable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT staged FROM dolt_status WHERE table_name = 'public.private_data';",
                    expected: Expected::Rows {
                        columns: &[Column("staged", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, secret FROM private_data;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("secret", VARCHAR)],
                        rows: &[
                            &[T("1"), T("secret")],
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
fn test_branches_system_table_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_branches basic usage",
            intent: "dolt_branches lists each branch with its head's committer and message, which move with a commit on the branch.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_commit_message FROM dolt_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_commit_message", TEXT)],
                        rows: &[
                            &[T("main"), T("postgres"), T("Initialize data repository")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'empty commit');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_commit_message FROM dolt_branches ORDER by name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_commit_message", TEXT)],
                        rows: &[
                            &[T("branch1"), T("postgres"), T("Initialize data repository")],
                            &[T("main"), T("postgres"), T("empty commit")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "User-created FKs to dolt_branches system table",
            intent: "A user table's foreign key to dolt_branches(name) is refused, since dolt_branches is a view.",
            set_up_script: &[
                "SELECT dolt_branch('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ext_branch_metadata(branch_name varchar(300) PRIMARY KEY, owner varchar(255), CONSTRAINT fk_branch_metadata_branch FOREIGN KEY (branch_name) REFERENCES dolt_branches(name));",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "referenced relation \"dolt_branches\" is not a table", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's system tables are views in Doltgrust, which Postgres' foreign keys can't reference (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "indexes cannot be created on dolt_branches",
            intent: "dolt_branches can't be indexed.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_dolt_branches_latest_committer ON dolt_branches(latest_commit_date);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot create index on relation \"dolt_branches\"", detail: "This operation is not supported for views.", ..E }),
                    dolt: Some(Dolt::Differs("dolt_branches is a view, which Postgres refuses to index with its own error (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "foreign keys referencing dolt_branches cannot use referential actions",
            intent: "A foreign key to dolt_branches is refused with any referential action, NO ACTION included, since dolt_branches is a view.",
            set_up_script: &[
                "CREATE TABLE ext_branch_metadata(branch_name varchar(300) PRIMARY KEY, owner varchar(255));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ext_branch_metadata ADD CONSTRAINT fk123 FOREIGN KEY (branch_name) REFERENCES dolt_branches(name) ON DELETE CASCADE;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "referenced relation \"dolt_branches\" is not a table", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's system tables are views in Doltgrust, which Postgres' foreign keys can't reference (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ext_branch_metadata ADD CONSTRAINT fk123 FOREIGN KEY (branch_name) REFERENCES dolt_branches(name) ON UPDATE CASCADE;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "referenced relation \"dolt_branches\" is not a table", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's system tables are views in Doltgrust, which Postgres' foreign keys can't reference (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ext_branch_metadata ADD CONSTRAINT fk123 FOREIGN KEY (branch_name) REFERENCES dolt_branches(name) ON DELETE RESTRICT;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "referenced relation \"dolt_branches\" is not a table", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's system tables are views in Doltgrust, which Postgres' foreign keys can't reference (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ext_branch_metadata ADD CONSTRAINT fk123 FOREIGN KEY (branch_name) REFERENCES dolt_branches(name) ON UPDATE RESTRICT;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "referenced relation \"dolt_branches\" is not a table", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's system tables are views in Doltgrust, which Postgres' foreign keys can't reference (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ext_branch_metadata ADD CONSTRAINT fk123 FOREIGN KEY (branch_name) REFERENCES dolt_branches(name) ON DELETE NO ACTION ON UPDATE NO ACTION;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "referenced relation \"dolt_branches\" is not a table", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's system tables are views in Doltgrust, which Postgres' foreign keys can't reference (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_branches.name index",
            intent: "dolt_branches filtered by name ranges returns the branches in each range.",
            set_up_script: &[
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "SELECT dolt_branch('branch3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name = 'branch1';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch1"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name > 'branch1';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch2"), T("postgres"), T("postgres@localhost")],
                            &[T("branch3"), T("postgres"), T("postgres@localhost")],
                            &[T("main"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name >= 'branch2';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch2"), T("postgres"), T("postgres@localhost")],
                            &[T("branch3"), T("postgres"), T("postgres@localhost")],
                            &[T("main"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name < 'branch2';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch1"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name <= 'branch2';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch1"), T("postgres"), T("postgres@localhost")],
                            &[T("branch2"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name <= 'branch3' AND name > 'branch1';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch2"), T("postgres"), T("postgres@localhost")],
                            &[T("branch3"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name <= 'branch2' AND name >= 'branch2';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch2"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name < 'branch3' AND name > 'branch2';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name > 'branch2';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch3"), T("postgres"), T("postgres@localhost")],
                            &[T("main"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name <= 'branch3' AND name > 'branch1' AND name > 'branch2';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch3"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name <= 'branch1' AND name > 'branch3';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_committer, latest_committer_email FROM dolt_branches WHERE name <= 'branch1' OR name > 'branch3';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT)],
                        rows: &[
                            &[T("branch1"), T("postgres"), T("postgres@localhost")],
                            &[T("main"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness makes its first commit as root with a checkpoint message; a fresh Doltgrust database's first commit is its initial commit, made by the session's user")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_branches latest_committer column when unset reflects --author",
            intent: "A commit made with --author shows that author as both committer and author in dolt_branches.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'author commit', '--author', 'Test Author <author@example.com>');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT latest_committer, latest_committer_email, latest_author, latest_author_email FROM dolt_branches WHERE name = 'feature';",
                    expected: Expected::Rows {
                        columns: &[Column("latest_committer", TEXT), Column("latest_committer_email", TEXT), Column("latest_author", TEXT), Column("latest_author_email", TEXT)],
                        rows: &[
                            &[T("Test Author"), T("author@example.com"), T("Test Author"), T("author@example.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_branches default commit populates committer and author from session identity",
            intent: "A commit made without --author shows the session's identity as committer and author in dolt_branches.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'session commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT latest_committer, latest_committer_email, latest_author, latest_author_email FROM dolt_branches WHERE name = 'feature';",
                    expected: Expected::Rows {
                        columns: &[Column("latest_committer", TEXT), Column("latest_committer_email", TEXT), Column("latest_author", TEXT), Column("latest_author_email", TEXT)],
                        rows: &[
                            &[T("postgres"), T("postgres@localhost"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness runs its sessions as root@localhost, where Doltgrust's script tests run as postgres")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_branches schema exposes split committer and author columns",
            intent: "dolt_branches has separate committer and author columns, with their dates.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name::text, data_type::text, is_nullable::text FROM information_schema.columns WHERE table_name = 'dolt_branches' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", TEXT), Column("data_type", TEXT), Column("is_nullable", TEXT)],
                        rows: &[
                            &[T("name"), T("text"), T("YES")],
                            &[T("hash"), T("text"), T("YES")],
                            &[T("latest_committer"), T("text"), T("YES")],
                            &[T("latest_committer_email"), T("text"), T("YES")],
                            &[T("latest_commit_date"), T("timestamp with time zone"), T("YES")],
                            &[T("latest_commit_message"), T("text"), T("YES")],
                            &[T("remote"), T("text"), T("YES")],
                            &[T("branch"), T("text"), T("YES")],
                            &[T("dirty"), T("boolean"), T("YES")],
                            &[T("latest_author"), T("text"), T("YES")],
                            &[T("latest_author_email"), T("text"), T("YES")],
                            &[T("latest_author_date"), T("timestamp with time zone"), T("YES")],
                        ],
                        tag: "SELECT 12",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's; information_schema.columns lists the same columns, where Postgres shows a view's columns as nullable and keyless")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_call_as_of() {
    run_scripts(&[
        ScriptTest {
            name: "Database syntax properly handles inter-CALL communication",
            intent: "A procedure called through a branch's revision qualifier runs that branch's procedures, the ones it calls included, while an unqualified call runs the current ones.",
            set_up_script: &[
                "CREATE PROCEDURE p2(OUT param TEXT) LANGUAGE plpgsql AS $$ BEGIN param := 'b'; END $$;",
                "CREATE PROCEDURE p1(OUT str TEXT) LANGUAGE plpgsql AS $$ BEGIN CALL p2(str); str := concat('a', str); END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'First procedures');",
                "SELECT dolt_branch('p12');",
                "DROP PROCEDURE p1;",
                "DROP PROCEDURE p2;",
                "CREATE PROCEDURE p2(OUT param TEXT) LANGUAGE plpgsql AS $$ BEGIN param := 'd'; END $$;",
                "CREATE PROCEDURE p1(OUT str TEXT) LANGUAGE plpgsql AS $$ BEGIN CALL p2(str); str := concat('c', str); END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'Second procedures');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL p1(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("str", TEXT)],
                        rows: &[
                            &[T("cd")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/main\".public.p1(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("str", TEXT)],
                        rows: &[
                            &[T("cd")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/p12\".public.p1(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("str", TEXT)],
                        rows: &[
                            &[T("ab")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL ... AS OF references historic data through nested calls",
            intent: "A procedure called AS OF a commit reads that commit's rows, through the procedures it calls too.",
            set_up_script: &[
                "CREATE TABLE test (v1 BIGINT);",
                "INSERT INTO test VALUES (1);",
                "CREATE PROCEDURE p1(OUT v1 BIGINT) LANGUAGE plpgsql AS $$ BEGIN CALL p2(v1); END $$;",
                "CREATE PROCEDURE p2(OUT v1 BIGINT) LANGUAGE plpgsql AS $$ BEGIN SELECT test.v1 INTO v1 FROM test; END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
                "UPDATE test SET v1 = 2;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
                "UPDATE test SET v1 = 3;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
                "UPDATE test SET v1 = 4;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL p1(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD~2';",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD~3';",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL ... AS OF doesn't overwrite nested CALL ... AS OF",
            intent: "A CALL or table read inside a procedure that names its own AS OF keeps it when the procedure itself is called AS OF another commit.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (pk BIGINT PRIMARY KEY, s TEXT);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "CREATE PROCEDURE p1(OUT r TEXT) LANGUAGE plpgsql AS $$ BEGIN CALL p2(r); END $$;",
                "CREATE PROCEDURE p1a(OUT r TEXT) LANGUAGE plpgsql AS $$ BEGIN CALL p2(r) AS OF 'HEAD~2'; END $$;",
                "CREATE PROCEDURE p1b(OUT r TEXT) LANGUAGE plpgsql AS $$ BEGIN CALL p2a(r); END $$;",
                "CREATE PROCEDURE p2(OUT r TEXT) LANGUAGE plpgsql AS $$ BEGIN SELECT string_agg(t::TEXT, '; ' ORDER BY pk) INTO r FROM myhistorytable t; END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
                "DROP TABLE myhistorytable;",
                "CREATE TABLE myhistorytable (pk BIGINT PRIMARY KEY, s TEXT, c TEXT);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "CREATE PROCEDURE p2a(OUT r TEXT) LANGUAGE plpgsql AS $$ BEGIN SELECT string_agg(t::TEXT, '; ' ORDER BY pk) INTO r FROM myhistorytable AS OF 'HEAD~1' t; END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL p1(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 3\",1); (2,\"second row, 3\",2); (3,\"third row, 3\",3)")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1a(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 1\"); (2,\"second row, 1\"); (3,\"third row, 1\")")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1b(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 2\"); (2,\"second row, 2\"); (3,\"third row, 2\")")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p2(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 3\",1); (2,\"second row, 3\",2); (3,\"third row, 3\",3)")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p2a(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 2\"); (2,\"second row, 2\"); (3,\"third row, 2\")")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD~2';",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 1\"); (2,\"second row, 1\"); (3,\"third row, 1\")")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1a(NULL) AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 1\"); (2,\"second row, 1\"); (3,\"third row, 1\")")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1b(NULL) AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 2\"); (2,\"second row, 2\"); (3,\"third row, 2\")")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p2(NULL) AS OF 'HEAD~2';",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 1\"); (2,\"second row, 1\"); (3,\"third row, 1\")")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p2a(NULL) AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("r", TEXT)],
                        rows: &[
                            &[T("(1,\"first row, 2\"); (2,\"second row, 2\"); (3,\"third row, 2\")")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its rows as one text of each row, in key order")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL ... AS OF errors if attempting to modify a table",
            intent: "A procedure called AS OF a commit can't write, since what it reads is history.",
            set_up_script: &[
                "CREATE TABLE test (v1 BIGINT);",
                "INSERT INTO test VALUES (2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
                "CREATE PROCEDURE p1() LANGUAGE plpgsql AS $$ BEGIN UPDATE test SET v1 = v1 * 2; END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1();",
                    expected: Expected::Tag("CALL"),
                    dolt: Some(Dolt::MySqlOnly("MySQL reports the rows the procedure's last statement changed, and Postgres' CALL reports none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1() AS OF 'HEAD~1';",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "CALL ... AS OF converts databases to read only", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Database syntax propagates to inner calls",
            intent: "A procedure called through a branch's revision qualifier calls that branch's procedures too.",
            set_up_script: &[
                "CREATE PROCEDURE p4(OUT n INT) LANGUAGE plpgsql AS $$ BEGIN CALL p5(n); END $$;",
                "CREATE PROCEDURE p5(OUT n INT) LANGUAGE plpgsql AS $$ BEGIN n := 3; END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
                "SELECT dolt_branch('p45');",
                "DROP PROCEDURE p4;",
                "DROP PROCEDURE p5;",
                "CREATE PROCEDURE p4(OUT n INT) LANGUAGE plpgsql AS $$ BEGIN CALL p5(n); END $$;",
                "CREATE PROCEDURE p5(OUT n INT) LANGUAGE plpgsql AS $$ BEGIN n := 4; END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL p4(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p5(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/main\".public.p4(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/main\".public.p5(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/p45\".public.p4(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/p45\".public.p5(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Database syntax with AS OF",
            intent: "A revision qualifier on a CALL runs that revision's procedure wholly in the revision, and AS OF picks the commit of the revision's history that its reads see.",
            set_up_script: &[
                "CREATE TABLE test (v1 BIGINT);",
                "INSERT INTO test VALUES (2);",
                "CREATE PROCEDURE p1(OUT r BIGINT) LANGUAGE plpgsql AS $$ BEGIN SELECT v1 * 10 INTO r FROM test; END $$;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
                "SELECT dolt_branch('other');",
                "DROP PROCEDURE p1;",
                "CREATE PROCEDURE p1(OUT r BIGINT) LANGUAGE plpgsql AS $$ BEGIN SELECT v1 * 100 INTO r FROM test; END $$;",
                "UPDATE test SET v1 = 3;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit message');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL p1(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("300")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/main\".public.p1(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("300")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/other\".public.p1(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("300")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/main\".public.p1(NULL) AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("300")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/other\".public.p1(NULL) AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p1(NULL) AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/main\".public.p1(NULL) AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT8)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "CALL",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL \"postgres/other\".public.p1(NULL) AS OF 'HEAD~1';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test\" does not exist", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL procedures return a result set, Postgres procedures their OUT parameters, so the procedure returns its value as one")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_views_with_as_of_script_test() {
    run_scripts(&[
        ScriptTest {
            name: "Querying a view with a union using an as of expression",
            intent: "A view read AS OF a commit runs its current definition over the relations as they were at that commit, failing when the commit lacks one of them.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'cm0');",
                "CREATE TABLE t1 (pk int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, c0 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm1');",
                "INSERT INTO t1 (c0) VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'cm2');",
                "CREATE TABLE t2 (pk int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, vc varchar(100));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm3');",
                "INSERT INTO t2 (vc) VALUES ('one'), ('two');",
                "SELECT dolt_commit('-am', 'cm4');",
                "CREATE VIEW v1 AS SELECT pk, c0::TEXT AS c0 FROM t1 UNION SELECT * FROM t2;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm5');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM v1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's UNION coerces an int and a varchar column to one type, and Postgres' needs the view to cast c0 to text")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF 'HEAD~2';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF 'HEAD~3';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t2\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres reports the missing relation the view reads (42P01), where MySQL reports the view as invalid")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF 'HEAD~4';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t2\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres reports the missing relation the view reads (42P01), where MySQL reports the view as invalid")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF 'HEAD~5';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t1\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres reports the missing relation the view reads (42P01), where MySQL reports the view as invalid")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF HEAD;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF HEAD.ASDF;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: HEAD.ASDF", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_describe_table_as_of_script_test() {
    run_scripts(&[
        ScriptTest {
            name: "Describe table as of",
            intent: "A table's columns read AS OF a commit are the columns it had at that commit, and none before it existed.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', dolt_commit('--allow-empty', '-m', 'before creating table a'), false);",
                "CREATE TABLE a (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table a'), false);",
                "ALTER TABLE a ADD COLUMN c2 varchar(20);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'adding column c2'), false);",
                "ALTER TABLE a DROP COLUMN c1;",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'dropping column c1'), false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, character_maximum_length, is_nullable FROM information_schema.columns AS OF current_setting('test.Commit0') WHERE table_name = 'a' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("is_nullable", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE of a missing table fails, and the information_schema query this port uses finds no rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, character_maximum_length, is_nullable FROM information_schema.columns AS OF current_setting('test.Commit1') WHERE table_name = 'a' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("integer"), Null, T("NO")],
                            &[T("c1"), T("integer"), Null, T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so the port reads information_schema.columns AS OF the commit, which has no key or extra column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, character_maximum_length, is_nullable FROM information_schema.columns AS OF current_setting('test.Commit2') WHERE table_name = 'a' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("integer"), Null, T("NO")],
                            &[T("c1"), T("integer"), Null, T("YES")],
                            &[T("c2"), T("character varying"), T("20"), T("YES")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so the port reads information_schema.columns AS OF the commit, which has no key or extra column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, character_maximum_length, is_nullable FROM information_schema.columns AS OF current_setting('test.Commit3') WHERE table_name = 'a' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("integer"), Null, T("NO")],
                            &[T("c2"), T("character varying"), T("20"), T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so the port reads information_schema.columns AS OF the commit, which has no key or extra column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, character_maximum_length, is_nullable FROM information_schema.columns AS OF HEAD WHERE table_name = 'a' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("integer"), Null, T("NO")],
                            &[T("c2"), T("character varying"), T("20"), T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so the port reads information_schema.columns AS OF the commit, which has no key or extra column")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_log_table_function_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "invalid arguments",
            intent: "dolt_log refuses NULL, malformed, and missing revisions and options with errors, and decorates refs as asked.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(NULL, NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(NULL, '--not', NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit1', TRUE), '--not', NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit1', TRUE), '--min-parents', NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit1', TRUE), '--min-parents', 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_log(text, unknown, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("dolt_log takes text arguments, so Postgres finds no function for an integer argument, where Dolt's table function refuses it as an invalid argument")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(123, current_setting('test.Commit1', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_log(integer, text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("dolt_log takes text arguments, so Postgres finds no function for an integer argument, where Dolt's table function refuses it as an invalid argument")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit1', TRUE), 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_log(text, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("dolt_log takes text arguments, so Postgres finds no function for an integer argument, where Dolt's table function refuses it as an invalid argument")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit1', TRUE), '--not', 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_log(text, unknown, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("dolt_log takes text arguments, so Postgres finds no function for an integer argument, where Dolt's table function refuses it as an invalid argument")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main..branch1', current_setting('test.Commit1', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('^main..branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('^main...branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit1', TRUE), 'main..branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit1', TRUE), 'main...branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main..branch1', '--not', current_setting('test.Commit1', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main...branch1', '--not', current_setting('test.Commit1', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main', '--not', '^branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main', '--not', 'main..branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('fake-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('^fake-branch', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('fake-branch', '^main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main..fake-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main', '--not', 'fake-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(concat('fake', '-', 'branch'));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    dolt: Some(Dolt::Differs("Dolt takes only literal arguments, a limit of its table functions; Doltgrust's dolt_log is a set-returning function that takes any expression")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(dolt_hashof('main'));",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt takes only literal arguments, a limit of its table functions; Doltgrust's dolt_log is a set-returning function that takes any expression")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit3', TRUE), '--not', dolt_hashof('main'));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    dolt: Some(Dolt::Differs("Dolt takes only literal arguments, a limit of its table functions; Doltgrust's dolt_log is a set-returning function that takes any expression")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log(current_setting('test.Commit1', TRUE), LOWER(current_setting('test.Commit2', TRUE)));",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt takes only literal arguments, a limit of its table functions; Doltgrust's dolt_log is a set-returning function that takes any expression")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parents FROM dolt_log();",
                    expected: Expected::Rows {
                        columns: &[Column("parents", TEXT)],
                        rows: &[
                            &[Null],
                            &[Null],
                            &[Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parents = current_setting('test.Commit1', TRUE) FROM dolt_log('--parents') WHERE commit_hash = current_setting('test.Commit2', TRUE);",
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
                    query: "SELECT * FROM dolt_log('--decorate', 'invalid');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('--decorate', 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_log(unknown, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("dolt_log takes text arguments, so Postgres finds no function for an integer argument, where Dolt's table function refuses it as an invalid argument")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('--decorate', NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log();",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> main")],
                            &[T("")],
                            &[T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log('--decorate', 'no') WHERE commit_hash = current_setting('test.Commit2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log('--decorate', 'auto') WHERE commit_hash = current_setting('test.Commit2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> main")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "NOTICE", code: "01P01", message: "--decorate=auto has no defined meaning in SQL contexts; defaulting to short", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with one revision",
            intent: "dolt_log of a branch, a commit, or a revision expression lists that revision's commits, newest first.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'new-branch');",
                "INSERT INTO t VALUES (3, 'three', 'four');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t again'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log();",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting into t")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting into t")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log(current_setting('test.Commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log(current_setting('test.Commit2', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting into t")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log(current_setting('test.Commit3', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting into t again")],
                            &[T("inserting into t")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting into t again")],
                            &[T("inserting into t")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main^');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main') CROSS JOIN dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE commit_hash = dolt_to_commit;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting into t")],
                            &[T("inserting into t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's JOIN without ON is a cross join, which Postgres writes CROSS JOIN")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with more than one revision or revision range",
            intent: "dolt_log of several revisions, ranges, and --not lists the commits reachable from the included revisions but not the excluded ones.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t 2'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'new-branch');",
                "INSERT INTO t VALUES (3, 'three', 'four');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t 3'), FALSE);",
                "INSERT INTO t VALUES (4, 'four', 'five');",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting into t 4'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 'five', 'six');",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'inserting into t 5'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('^main', 'new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('main..new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('main...new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('new-branch', '--not', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('new-branch', '^main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('^new-branch', 'main');",
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
                    query: "SELECT count(*) FROM dolt_log('main', '--not', 'new-branch');",
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
                    query: "SELECT count(*) FROM dolt_log('^main', 'main');",
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
                    query: "SELECT count(*) FROM dolt_log('main..main');",
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
                    query: "SELECT count(*) FROM dolt_log('main...main');",
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
                    query: "SELECT count(*) FROM dolt_log('main', '--not', 'main');",
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
                    query: "SELECT count(*) FROM dolt_log('^main~', 'main');",
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
                    query: "SELECT count(*) FROM dolt_log('^main^', 'main');",
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
                    query: "SELECT count(*) FROM dolt_log('^main', 'main^');",
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
                    query: "SELECT count(*) FROM dolt_log('^main', current_setting('test.Commit3', TRUE));",
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
                    query: "SELECT count(*) FROM dolt_log('^new-branch', current_setting('test.Commit5', TRUE));",
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
                    query: "SELECT count(*) FROM dolt_log(current_setting('test.Commit3', TRUE), '--not', current_setting('test.Commit2', TRUE));",
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
                    query: "SELECT count(*) FROM dolt_log(current_setting('test.Commit4', TRUE), '--not', current_setting('test.Commit2', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('^main', '^new-branch');",
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
                    query: "SELECT count(*) FROM dolt_log('^main', '--not', 'new-branch');",
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
            name: "basic case with one revision, row content",
            intent: "dolt_log's rows carry each commit's hash, committer, author, and message.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'new-branch');",
                "INSERT INTO t VALUES (3, 'three', 'four');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t again', '--author', 'John Doe <johndoe@example.com>'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit2', TRUE), commit_hash = current_setting('test.Commit1', TRUE), committer, email, message FROM dolt_log();",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("f"), T("postgres"), T("postgres@localhost"), T("inserting into t")],
                            &[T("f"), T("t"), T("postgres"), T("postgres@localhost"), T("creating table t")],
                            &[T("f"), T("f"), T("postgres"), T("postgres@localhost"), T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit2', TRUE), committer, email, message FROM dolt_log('main') LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("postgres"), T("postgres@localhost"), T("inserting into t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit3', TRUE), committer, email, author, author_email, message FROM dolt_log('new-branch') LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t again")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit1', TRUE), committer, email, message FROM dolt_log(current_setting('test.Commit1', TRUE)) LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("postgres"), T("postgres@localhost"), T("creating table t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with more than one revision or revision range, row content",
            intent: "dolt_log's rows for revision ranges carry each commit's hash, committer, author, and message.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t 2'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'new-branch');",
                "INSERT INTO t VALUES (3, 'three', 'four');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t 3', '--author', 'John Doe <johndoe@example.com>'), FALSE);",
                "INSERT INTO t VALUES (4, 'four', 'five');",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting into t 4', '--author', 'John Doe <johndoe@example.com>'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 'five', 'six');",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'inserting into t 5'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit4', TRUE), commit_hash = current_setting('test.Commit3', TRUE), committer, email, author, author_email, message FROM dolt_log('^main', 'new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("f"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 4")],
                            &[T("f"), T("t"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit4', TRUE), commit_hash = current_setting('test.Commit3', TRUE), committer, email, author, author_email, message FROM dolt_log('main..new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("f"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 4")],
                            &[T("f"), T("t"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit5', TRUE), commit_hash = current_setting('test.Commit4', TRUE), commit_hash = current_setting('test.Commit3', TRUE), committer, email, author, author_email, message FROM dolt_log('main...new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("f"), T("t"), T("f"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 4")],
                            &[T("t"), T("f"), T("f"), T("postgres"), T("postgres@localhost"), T("postgres"), T("postgres@localhost"), T("inserting into t 5")],
                            &[T("f"), T("f"), T("t"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 3")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit4', TRUE), commit_hash = current_setting('test.Commit3', TRUE), committer, email, author, author_email, message FROM dolt_log('new-branch', '--not', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("f"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 4")],
                            &[T("f"), T("t"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit4', TRUE), commit_hash = current_setting('test.Commit3', TRUE), committer, email, author, author_email, message FROM dolt_log('new-branch', '^main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("f"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 4")],
                            &[T("f"), T("t"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit5', TRUE), committer, email, message FROM dolt_log('^new-branch', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("postgres"), T("postgres@localhost"), T("inserting into t 5")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('^main', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("committer", TEXT), Column("email", TEXT), Column("date", TIMESTAMPTZ), Column("message", TEXT), Column("commit_order", INT8), Column("parents", TEXT), Column("refs", TEXT), Column("signature", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMPTZ)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit5', TRUE), committer, email, message FROM dolt_log('^main~', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("postgres"), T("postgres@localhost"), T("inserting into t 5")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit5', TRUE), committer, email, message FROM dolt_log( 'main', '--not', 'main~');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("postgres"), T("postgres@localhost"), T("inserting into t 5")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit3', TRUE), committer, email, author, author_email, message FROM dolt_log('^main', current_setting('test.Commit3', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit3', TRUE), committer, email, author, author_email, message FROM dolt_log(current_setting('test.Commit3', TRUE), '--not', current_setting('test.Commit2', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("John Doe"), T("johndoe@example.com"), T("John Doe"), T("johndoe@example.com"), T("inserting into t 3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit5', TRUE), committer, email, message FROM dolt_log('^new-branch', current_setting('test.Commit5', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("postgres"), T("postgres@localhost"), T("inserting into t 5")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit5', TRUE), committer, email, message FROM dolt_log(current_setting('test.Commit5', TRUE), '--not', current_setting('test.Commit4', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("postgres"), T("postgres@localhost"), T("inserting into t 5")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple revisions",
            intent: "dolt_log of several branches and tags lists their commits once each, newest first, leaving out what ^ excludes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit 1 MAIN [1M]');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 2 MAIN [2M]');",
                "SELECT dolt_tag('tagM');",
                "SELECT * FROM dolt_checkout('-b', 'branchA');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 BRANCHA [1A]');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 2 BRANCHA [2A]');",
                "SELECT * FROM dolt_checkout('-b', 'branchB');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 BRANCHB [1B]');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 3 BRANCHA [3A]');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 3 AFTER [3M]');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'branchA');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 3 BRANCHA [3A]")],
                            &[T("commit 1 BRANCHB [1B]")],
                            &[T("commit 2 BRANCHA [2A]")],
                            &[T("commit 1 BRANCHA [1A]")],
                            &[T("commit 2 MAIN [2M]")],
                            &[T("commit 1 MAIN [1M]")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main', 'branchA');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 3 BRANCHA [3A]")],
                            &[T("commit 2 BRANCHA [2A]")],
                            &[T("commit 3 AFTER [3M]")],
                            &[T("commit 1 BRANCHA [1A]")],
                            &[T("commit 2 MAIN [2M]")],
                            &[T("commit 1 MAIN [1M]")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main', 'branchB', 'branchA');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 3 BRANCHA [3A]")],
                            &[T("commit 1 BRANCHB [1B]")],
                            &[T("commit 2 BRANCHA [2A]")],
                            &[T("commit 3 AFTER [3M]")],
                            &[T("commit 1 BRANCHA [1A]")],
                            &[T("commit 2 MAIN [2M]")],
                            &[T("commit 1 MAIN [1M]")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'main', '^branchA');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB [1B]")],
                            &[T("commit 3 AFTER [3M]")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'main', '--not', 'branchA');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB [1B]")],
                            &[T("commit 3 AFTER [3M]")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'main', '^branchA', '^main');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB [1B]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('tagM..branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB [1B]")],
                            &[T("commit 2 BRANCHA [2A]")],
                            &[T("commit 1 BRANCHA [1A]")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('HEAD..branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB [1B]")],
                            &[T("commit 2 BRANCHA [2A]")],
                            &[T("commit 1 BRANCHA [1A]")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table names given",
            intent: "dolt_log --tables lists only the commits that changed the given tables, across merges and drops.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test [1M]');",
                "CREATE TABLE test2 (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test2 [2M]');",
                "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test VALUES (0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'inserted 0 into test [1TB]');",
                "CREATE TABLE test3 (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test3 [2TB]');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'inserted 1 into test [3M]');",
                "SELECT * FROM dolt_merge('test-branch', '-m', 'merged test-branch [4M]');",
                "DROP TABLE test3;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'dropped table test3 [5M]');",
                "INSERT INTO test VALUES (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'inserted 2 into test [6M]');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--tables', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserted 2 into test [6M]")],
                            &[T("merged test-branch [4M]")],
                            &[T("inserted 1 into test [3M]")],
                            &[T("inserted 0 into test [1TB]")],
                            &[T("created table test [1M]")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--tables', 'test2');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("created table test2 [2M]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--tables', 'test3');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("dropped table test3 [5M]")],
                            &[T("created table test3 [2TB]")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--tables', 'test,test2');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserted 2 into test [6M]")],
                            &[T("merged test-branch [4M]")],
                            &[T("inserted 1 into test [3M]")],
                            &[T("inserted 0 into test [1TB]")],
                            &[T("created table test2 [2M]")],
                            &[T("created table test [1M]")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('test-branch', '--tables', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserted 0 into test [1TB]")],
                            &[T("created table test [1M]")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "min parents, merges, show parents, decorate",
            intent: "dolt_log --merges and --min-parents list merge commits, --parents shows their parents, and --decorate shows refs and tags.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES(0,0);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting 0,0'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "INSERT INTO t VALUES(1,1);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting 1,1'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
                "SELECT * FROM dolt_merge('branch2');",
                "SELECT dolt_tag('v1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT committer, email, message FROM dolt_log('--merges');",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("root"), T("root@localhost"), T("Merge branch 'branch2' into main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT committer, email, message FROM dolt_log('--min-parents', '2');",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("root"), T("root@localhost"), T("Merge branch 'branch2' into main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT committer, email, message FROM dolt_log('main', '--min-parents', '2');",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("root"), T("root@localhost"), T("Merge branch 'branch2' into main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('main', '--min-parents', '1');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('main', '--min-parents', '1', '--merges');",
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
                    query: "SELECT committer, email, message FROM dolt_log('branch1..main', '--min-parents', '2');",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("root"), T("root@localhost"), T("Merge branch 'branch2' into main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('--min-parents', '5');",
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
                    query: "SELECT message, split_part(parents, ', ', 1) = current_setting('test.Commit2', TRUE), split_part(parents, ', ', 2) = current_setting('test.Commit3', TRUE) FROM dolt_log('main', '--parents', '--merges');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("Merge branch 'branch2' into main"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SUBSTRING_INDEX is MySQL's; split_part takes a merge's first and second parents")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parents IS NULL FROM dolt_log('main', '--merges') LIMIT 1;",
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
                    query: "SELECT commit_hash = current_setting('test.Commit3', TRUE), parents = current_setting('test.Commit1', TRUE) FROM dolt_log('branch2', '--parents') LIMIT 1;",
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
                    query: "SELECT message, split_part(parents, ', ', 1) = current_setting('test.Commit2', TRUE), split_part(parents, ', ', 2) = current_setting('test.Commit3', TRUE) FROM dolt_log('branch1..main', '--parents', '--merges') LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("Merge branch 'branch2' into main"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SUBSTRING_INDEX is MySQL's; split_part takes a merge's first and second parents")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit2', TRUE), parents = current_setting('test.Commit1', TRUE) FROM dolt_log('branch2..branch1', '--parents') LIMIT 1;",
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
                    query: "SELECT refs FROM dolt_log('--decorate', 'short') LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> main, tag: v1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log('--decorate', 'full') LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> refs/heads/main, tag: refs/tags/v1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.Commit2', TRUE), parents = current_setting('test.Commit1', TRUE), refs FROM dolt_log('branch2..branch1', '--parents', '--decorate', 'short') LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("refs", TEXT)],
                        rows: &[
                            &[T("t"), T("t"), T("HEAD -> branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("Dolt's harness commits as root, where Doltgrust's session commits as postgres"),
            ..S
        },
        ScriptTest {
            name: "commit_order column in dolt_log system table and function",
            intent: "dolt_log's commit_order numbers commits in topological order, with refs on the newest.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.FeatureCommit', ('')::text, FALSE);",
                "SELECT set_config('test.MainCommit', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'commit 1'), FALSE);",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 2');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "SELECT set_config('test.FeatureCommit', dolt_commit('--allow-empty', '-m', 'feature commit'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT set_config('test.MainCommit', dolt_commit('--allow-empty', '-m', 'main commit'), FALSE);",
                "SELECT * FROM dolt_merge('feature', '-m', 'merge feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'dolt_log' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("commit_hash"), T("text"), T("YES")],
                            &[T("committer"), T("text"), T("YES")],
                            &[T("email"), T("text"), T("YES")],
                            &[T("date"), T("timestamp with time zone"), T("YES")],
                            &[T("message"), T("text"), T("YES")],
                            &[T("commit_order"), T("bigint"), T("YES")],
                            &[T("parents"), T("text"), T("YES")],
                            &[T("refs"), T("text"), T("YES")],
                            &[T("signature"), T("text"), T("YES")],
                            &[T("author"), T("text"), T("YES")],
                            &[T("author_email"), T("text"), T("YES")],
                            &[T("author_date"), T("timestamp with time zone"), T("YES")],
                        ],
                        tag: "SELECT 12",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's; information_schema.columns lists dolt_log's columns, a view's columns all nullable, with Doltgrust's types and author_date")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_order FROM dolt_log WHERE message = 'commit 1';",
                    expected: Expected::Rows {
                        columns: &[Column("commit_order", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log WHERE message = 'commit 1';",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log WHERE commit_order = (SELECT max(commit_order) FROM dolt_log);",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parents = concat(current_setting('test.MainCommit', TRUE), ', ', current_setting('test.FeatureCommit', TRUE)) FROM dolt_log WHERE message = 'merge feature';",
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
                    query: "SELECT signature FROM dolt_log WHERE message = 'commit 1';",
                    expected: Expected::Rows {
                        columns: &[Column("signature", TEXT)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_order FROM dolt_log() WHERE message = 'commit 1';",
                    expected: Expected::Rows {
                        columns: &[Column("commit_order", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT commit_order FROM dolt_log WHERE message = 'commit 1') = (SELECT commit_order FROM dolt_log() WHERE message = 'commit 1') AS same_order;",
                    expected: Expected::Rows {
                        columns: &[Column("same_order", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(DISTINCT commit_order) = 6 AS has_six_orders FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("has_six_orders", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM (SELECT commit_order, count(*) AS cnt FROM dolt_log GROUP BY commit_order HAVING count(*) > 1) AS duplicate_orders;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's HAVING may name a select-list alias; Postgres repeats the aggregate")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_log function in detached head state",
            intent: "dolt_log works while a tag is checked out in a detached head state.",
            set_up_script: &[
                "CREATE TABLE test_table (id int PRIMARY KEY, name VARCHAR(50));",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT dolt_tag('mytag');",
                "INSERT INTO test_table VALUES (1, 'test');",
                "SELECT dolt_commit('-Am', 'add data');",
                "SELECT dolt_tag('v2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log('mytag');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "mytag",
                    database: "postgres/mytag",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log('v2');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "v2",
                    database: "postgres/v2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('mytag') ORDER BY commit_order;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("create table")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "v2",
                    database: "postgres/v2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_log with non-literal arguments",
            intent: "dolt_log takes a column of an outer query as its revision, through LATERAL and in a correlated subquery.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dl1.message, dl2.message FROM dolt_log() dl1 JOIN LATERAL (SELECT message FROM dolt_log(dl1.commit_hash)) dl2 ON TRUE;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("inserting into t"), T("inserting into t")],
                            &[T("inserting into t"), T("creating table t")],
                            &[T("inserting into t"), T("Initialize data repository")],
                            &[T("creating table t"), T("creating table t")],
                            &[T("creating table t"), T("Initialize data repository")],
                            &[T("Initialize data repository"), T("Initialize data repository")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's JOIN LATERAL without ON is a cross join, which Postgres writes with ON TRUE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log() dl1 WHERE EXISTS (SELECT 1 FROM dolt_log(dl1.commit_hash, '--not', current_setting('test.Commit1', TRUE)));",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting into t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_log shows author and committer columns separately",
            intent: "dolt_log shows a commit's author and committer separately when they differ.",
            set_up_script: &[
                "CREATE TABLE log_author_tbl (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'author commit', '--author', 'Test Author <author@example.com>');",
                "SET dolt_committer_name = 'Committer';",
                "SELECT dolt_commit('--allow-empty', '-m', 'committer name commit', '--author', 'Test Author <author@example.com>');",
                "SET dolt_committer_email = 'committer@example.com';",
                "SELECT dolt_commit('--allow-empty', '-m', 'committer commit', '--author', 'Test Author <author@example.com>');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT committer, email, author, author_email FROM dolt_log ORDER BY date DESC LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Committer"), T("committer@example.com"), T("Test Author"), T("author@example.com")],
                            &[T("Committer"), T("postgres@localhost"), T("Test Author"), T("author@example.com")],
                            &[T("Test Author"), T("author@example.com"), T("Test Author"), T("author@example.com")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_temp_table_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "temporary and non-temporary table name collisions",
            intent: "A temporary table hides a permanent table of the same name until it is dropped, and a second temporary table of one name is refused.",
            set_up_script: &[
                "CREATE TABLE t1 (id varchar(100));",
                "INSERT INTO t1 VALUES ('this is a non-temporary table');",
                "CREATE TEMPORARY TABLE t1 (id varchar(100));",
                "INSERT INTO t1 VALUES ('this is a temporary table');",
                "CREATE TEMPORARY TABLE t2 (id int);",
                "CREATE TABLE t2 (id int);",
                "CREATE TEMPORARY TABLE t3 (id int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR)],
                        rows: &[
                            &[T("this is a temporary table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE t3 (id int);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"t3\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE oid = 't1'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_class's relpersistence shows whether t1 names the temporary table or the permanent one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE oid = 't1'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("p")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_class's relpersistence shows whether t1 names the temporary table or the permanent one")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "temporary table supports auto increment",
            intent: "A temporary table's generated key numbers inserted rows, with explicit keys allowed beside them.",
            set_up_script: &[
                "CREATE TEMPORARY TABLE t (i int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                    expected: Expected::Tag("INSERT 0 3"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's empty VALUES rows are written VALUES (DEFAULT); SHOW CREATE TABLE's AUTO_INCREMENT checks are left out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
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
                    query: "INSERT INTO t VALUES (100), (1000);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                    expected: Expected::Tag("INSERT 0 3"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's empty VALUES rows are written VALUES (DEFAULT); SHOW CREATE TABLE's AUTO_INCREMENT checks are left out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("100")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::Differs("a Postgres identity column's sequence never moves past explicit keys, so the next rows get 4, 5, and 6, where MySQL's AUTO_INCREMENT continues after 1000")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "temporary table tag collision",
            intent: "A temporary table can have more columns than a permanent table without their column ids colliding.",
            set_up_script: &[
                "CREATE TABLE note(a int, b int, userid int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE tmp_tbl(a int, b int, c int, d int, e int, f int, g int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop temporary table behavior",
            intent: "DROP TEMPORARY TABLE drops only temporary tables, leaving a permanent table of the same name.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "CREATE TEMPORARY TABLE tmp (i int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "show CREATE TABLE tmp;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"CREATE\"", position: 6, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEMPORARY TABLE tmp;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"TEMPORARY\"", position: 6, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE t (i int, j int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "show CREATE TABLE t;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"CREATE\"", position: 6, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEMPORARY TABLE t;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"TEMPORARY\"", position: 6, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "show CREATE TABLE t;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"CREATE\"", position: 6, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEMPORARY TABLE t;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "sql.ErrUnknownTable", ..E }),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "temporary tables in stored procedures",
            intent: "A temporary table a stored procedure creates stays after the call.",
            set_up_script: &[
                "CREATE procedure proc()\nBEGIN\n  CREATE TEMPORARY TABLE tmp (i int PRIMARY KEY);\n  INSERT INTO tmp VALUES (1), (2), (3);\n  SELECT * FROM tmp;\nEND;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT proc();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function proc() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tmp;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"tmp\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
    ]);
}

#[test]
fn test_dolt_info_schema_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "info_schema changes with dolt_checkout",
            intent: "information_schema.columns follows dolt_checkout to the checked-out branch's columns.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "SELECT * FROM dolt_checkout('b2');",
                "ALTER TABLE t ADD COLUMN c int;",
                "SELECT dolt_commit('-am', 'added column c on branch b2');",
                "SELECT * FROM dolt_checkout('b3');",
                "ALTER TABLE t ADD COLUMN d int;",
                "SELECT dolt_commit('-am', 'added column d on branch b3');",
                "SELECT * FROM dolt_checkout('main');",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's database is information_schema's table_schema; in Postgres the tables' schema is public")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's database is information_schema's table_schema; in Postgres the tables' schema is public")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's database is information_schema's table_schema; in Postgres the tables' schema is public")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "info_schema does not change with USE",
            intent: "information_schema.columns follows dolt_checkout, and a revision database's information_schema shows that branch's columns.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "ALTER TABLE \"postgres/b2\".public.t ADD COLUMN c int;",
                "ALTER TABLE \"postgres/b3\".public.t ADD COLUMN d int;",
                "SELECT * FROM dolt_checkout('main');",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* main */ SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's database is information_schema's table_schema; in Postgres the tables' schema is public")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* b2 */ SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("MySQL's database is information_schema's table_schema, so the port filters on public; and information_schema reads the branch the session has checked out, as Postgres' reads the catalog of the database it is connected to, where Dolt's shows the database's default branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM \"postgres/b2\".information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* b3 */ SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("MySQL's database is information_schema's table_schema, so the port filters on public; and information_schema reads the branch the session has checked out, as Postgres' reads the catalog of the database it is connected to, where Dolt's shows the database's default branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM \"postgres/b3\".information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "info_schema when checked out branch was deleted",
            intent: "information_schema works after the session's previously checked-out branch is deleted, and a deleted branch's revision database shows no columns.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "ALTER TABLE \"postgres/b2\".public.t ADD COLUMN c int;",
                "SELECT dolt_branch('b3');",
                "SELECT * FROM dolt_checkout('b3');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT dolt_branch('-df', 'b3');",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* main */ SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's database is information_schema's table_schema; in Postgres the tables' schema is public")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* b2 */ SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("MySQL's database is information_schema's table_schema, so the port filters on public; and information_schema reads the branch the session has checked out, as Postgres' reads the catalog of the database it is connected to, where Dolt's shows the database's default branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM \"postgres/b2\".information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b3\".information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"postgres/b3\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("a revision name of a deleted branch names no database, so Postgres refuses it as a missing database, where Dolt reads it as empty")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "info_schema with detached HEAD",
            intent: "information_schema.columns of a tag's revision database shows the columns at that tag.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "SELECT * FROM dolt_checkout('b2');",
                "ALTER TABLE t ADD COLUMN c int;",
                "SELECT dolt_commit('-am', 'added column c on branch b2');",
                "SELECT dolt_tag('t2');",
                "SELECT * FROM dolt_checkout('b3');",
                "ALTER TABLE t ADD COLUMN d int;",
                "SELECT dolt_commit('-am', 'added column d on branch b3');",
                "SELECT dolt_tag('t3');",
                "SELECT * FROM dolt_checkout('main');",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's database is information_schema's table_schema; in Postgres the tables' schema is public")),
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
                    client: "t2",
                    database: "postgres/t2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "t2",
                    database: "postgres/t2",
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
                    client: "t3",
                    database: "postgres/t3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_schema = 'public' AND table_name = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "t3",
                    database: "postgres/t3",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_auto_increment_tests() {
    run_scripts(&[
        ScriptTest {
            name: "insert on different branches",
            intent: "Inserts on different branches take generated keys from one counter, so no two branches hand out the same key.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'two values on main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (3), (4);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'two values on branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (5), (6);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop table",
            intent: "A table's generated keys keep counting across branches after the table is dropped on some of them and created again.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'two values on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (b) VALUES (3), (4);",
                "SELECT dolt_commit('-am', 'two values on branch1');",
                "SELECT * FROM dolt_checkout('branch2');",
                "INSERT INTO t (b) VALUES (5), (6);",
                "SELECT * FROM dolt_checkout('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (7), (8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (7), (8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                            &[T("9"), T("7")],
                            &[T("10"), T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Postgres' identity sequences never move back and never jump on their own: values continue after every value used on any branch sharing the table's storage, explicit keys don't move them, and RESTART WITH sets the next value exactly, where Dolt's AUTO_INCREMENT tracker recomputes from the rows the branches hold")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete all rows in table",
            intent: "Deleting every row and restarting the counter at 1 hands out keys from 1 again.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'two values on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a RESTART WITH 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (7), (8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("7")],
                            &[T("2"), T("8")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "set auto-increment below current max value",
            intent: "Setting the counter below the largest key makes later inserts collide with existing keys, and a later setting above an explicit key continues from there.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "INSERT INTO t (b) VALUES (1), (2), (3), (4);",
                "SELECT dolt_commit('-am', 'two values on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a RESTART WITH 2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (5), (6);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (a)=(2) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    dolt: Some(Dolt::Differs("Postgres' identity sequences never move back and never jump on their own: values continue after every value used on any branch sharing the table's storage, explicit keys don't move them, and RESTART WITH sets the next value exactly, where Dolt's AUTO_INCREMENT tracker recomputes from the rows the branches hold")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Postgres' identity sequences never move back and never jump on their own: values continue after every value used on any branch sharing the table's storage, explicit keys don't move them, and RESTART WITH sets the next value exactly, where Dolt's AUTO_INCREMENT tracker recomputes from the rows the branches hold")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (a, b) VALUES (100, 100);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a RESTART WITH 50;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE a >= 100 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("100"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres' identity sequences never move back and never jump on their own: values continue after every value used on any branch sharing the table's storage, explicit keys don't move them, and RESTART WITH sets the next value exactly, where Dolt's AUTO_INCREMENT tracker recomputes from the rows the branches hold")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "set auto-increment above current max value",
            intent: "Setting the counter above the largest key on several branches makes inserts continue from the highest setting.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "INSERT INTO t (b) VALUES (1), (2), (3), (4);",
                "SELECT dolt_commit('-am', 'two values on main');",
                "SELECT dolt_branch('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a RESTART WITH 20;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/branch1\".public.t (b) VALUES (5), (6);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (5), (6);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("20"), T("5")],
                            &[T("21"), T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("a branch keeps its sequence's storage once it changes the counter, so another branch's inserts don't move it, where Dolt's AUTO_INCREMENT tracker is shared by every branch (Appendix C, sequences after dolt_reset --hard)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete all rows in table in all branches",
            intent: "After every branch's rows are deleted and each counter restarted at 1, keys start from 1 again.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'two values on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (b) VALUES (3), (4);",
                "SELECT dolt_commit('-am', 'two values on branch1');",
                "SELECT * FROM dolt_checkout('branch2');",
                "INSERT INTO t (b) VALUES (5), (6);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/branch1\".public.t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/branch2\".public.t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/branch1\".public.t ALTER COLUMN a RESTART WITH 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/branch2\".public.t ALTER COLUMN a RESTART WITH 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a RESTART WITH 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (7), (8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("7")],
                            &[T("2"), T("8")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete all rows in table in all but one branch",
            intent: "After all but one branch's rows are deleted and the counters restarted, keys continue after the remaining branch's largest key.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'two values on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (b) VALUES (3), (4);",
                "SELECT dolt_commit('-am', 'two values on branch1');",
                "SELECT * FROM dolt_checkout('branch2');",
                "INSERT INTO t (b) VALUES (5), (6);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/branch1\".public.t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/branch2\".public.t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a RESTART WITH 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/branch2\".public.t ALTER COLUMN a RESTART WITH 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (5), (6);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                            &[T("2"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("a branch keeps its sequence's storage once it changes the counter, so another branch's inserts don't move it, where Dolt's AUTO_INCREMENT tracker is shared by every branch (Appendix C, sequences after dolt_reset --hard)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "truncate table in all branches",
            intent: "After the table is truncated on every branch, keys start over.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'two values on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (b) VALUES (3), (4);",
                "SELECT dolt_commit('-am', 'two values on branch1');",
                "SELECT * FROM dolt_checkout('branch2');",
                "INSERT INTO t (b) VALUES (5), (6);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "TRUNCATE t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE \"postgres/branch1\".public.t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE \"postgres/branch2\".public.t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a RESTART WITH 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... AUTO_INCREMENT = n is ALTER COLUMN ... RESTART WITH n for a Postgres identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (7), (8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("7")],
                            &[T("2"), T("8")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "truncate table",
            intent: "After the table is truncated on some branches, keys continue after the rows the other branches still hold.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'empty table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'two values on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (b) VALUES (3), (4);",
                "SELECT dolt_commit('-am', 'two values on branch1');",
                "SELECT * FROM dolt_checkout('branch2');",
                "INSERT INTO t (b) VALUES (5), (6);",
                "SELECT * FROM dolt_checkout('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (7), (8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (7), (8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                            &[T("9"), T("7")],
                            &[T("10"), T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Postgres' identity sequences never move back and never jump on their own: values continue after every value used on any branch sharing the table's storage, explicit keys don't move them, and RESTART WITH sets the next value exactly, where Dolt's AUTO_INCREMENT tracker recomputes from the rows the branches hold")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("11"), T("1")],
                            &[T("12"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Postgres' identity sequences never move back and never jump on their own: values continue after every value used on any branch sharing the table's storage, explicit keys don't move them, and RESTART WITH sets the next value exactly, where Dolt's AUTO_INCREMENT tracker recomputes from the rows the branches hold")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop auto_increment primary key",
            intent: "Dropping the generated primary key leaves the rows, and a new generated key can be added after.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int); CREATE INDEX  ON t (a);",
                "SELECT dolt_commit('-Am', 'empty table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'two values on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (b) VALUES (3), (4);",
                "SELECT dolt_commit('-am', 'two values on branch1');",
                "SELECT * FROM dolt_checkout('branch2');",
                "INSERT INTO t (b) VALUES (5), (6);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (7), (8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (9), (10);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                            &[T("9"), T("9")],
                            &[T("10"), T("10")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (b) VALUES (11), (12);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                            &[T("9"), T("9")],
                            &[T("10"), T("10")],
                            &[T("11"), T("11")],
                            &[T("12"), T("12")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hard reset dropped table restores auto increment",
            intent: "dolt_reset --hard restores a dropped table with a counter that continues after its rows.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'initialize table');",
                "DROP TABLE t;",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t(b) VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hard reset inserted rows continues auto increment",
            intent: "After dolt_reset --hard discards inserted rows, keys continue after the discarded ones.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'initialize table');",
                "INSERT INTO t (b) VALUES (3), (4);",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t(b) VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hard reset dropped table with branch restores auto increment",
            intent: "dolt_reset --hard restores a dropped table whose counter continues after another branch's explicit key.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, b int);",
                "INSERT INTO t (b) VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'initialize table');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (100, 100);",
                "SELECT dolt_commit('-Am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
                "DROP TABLE t;",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t(b) VALUES (101);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("101")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Postgres' identity sequences never move back and never jump on their own: values continue after every value used on any branch sharing the table's storage, explicit keys don't move them, and RESTART WITH sets the next value exactly, where Dolt's AUTO_INCREMENT tracker recomputes from the rows the branches hold")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_tag_test_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt-tag: SQL create tags",
            intent: "dolt_tag makes a tag of a commit with its tagger and message, and refuses a name a tag has.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-am','created table test');",
            ],
            assertions: &[
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
                    query: "SELECT dolt_tag('v1', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A tag named 'v1' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A tag named 'v1' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name, CASE WHEN CHAR_LENGTH(tag_hash) < 0 THEN NULL ELSE 'not null' END, tagger, email, CASE WHEN date IS NULL THEN NULL ELSE 'not null' END, message FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("case", TEXT), Column("tagger", TEXT), Column("email", TEXT), Column("case", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("v1"), T("not null"), T("postgres"), T("postgres@localhost"), T("not null"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness runs as root@localhost, and the Doltgrust harness's user is postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v2', '-m', 'create tag v2');",
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
                    query: "SELECT dolt_tag('v2', '-m', 'replace tag v2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A tag named 'v2' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name, message FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("v1"), T("")],
                            &[T("v2"), T("create tag v2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt-tag: SQL delete tags",
            intent: "dolt_tag -d deletes the tags it names.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-am','created table test');",
                "SELECT dolt_tag('v1', '-m', 'create tag v1');",
                "SELECT dolt_tag('v2', '-m', 'create tag v2');",
                "SELECT dolt_tag('v3', '-m', 'create tag v3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT tag_name, message FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("v1"), T("create tag v1")],
                            &[T("v2"), T("create tag v2")],
                            &[T("v3"), T("create tag v3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('-d','v1');",
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
                    query: "SELECT tag_name, message FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("v2"), T("create tag v2")],
                            &[T("v3"), T("create tag v3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('-d','v2','v3');",
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
                    query: "SELECT tag_name, message FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("message", TEXT)],
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
            name: "dolt-tag: SQL use a tag as a ref for merge",
            intent: "A tag names its commit wherever a revision is read, as in a merge.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-am','created table test');",
                "DELETE FROM test WHERE pk = 0;",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-am','made changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1','HEAD');",
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
                    query: "SELECT * FROM dolt_checkout('-b','other','HEAD^');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (8), (9);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am','made changes in other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('v1');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt-tag: case insensitive",
            intent: "A tag's name keeps its case, and a connection to the tag reads its commit by a name of any case.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am','created table test');",
                "SELECT dolt_tag('ABC');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-am','inserted rows into test');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("ABC")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    database: "postgres/abc",
                    skip: Some("Postgres' database names are case-sensitive, so postgres/abc names no tag ABC, where MySQL's database names ignore case; a connection names a tag in its exact case"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt-tag: checkout errors",
            intent: "Checking out a tag is refused, since a session works on a branch.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am','created table test');",
                "SELECT dolt_tag('v1');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-am','inserted rows into test');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("v1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('v1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "dolt does not support a detached head state. To create a branch at this tag, run: \n	CALL DOLT_CHECKOUT('v1', '-b', <new_branch_name>)", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt-tag: a branch and a tag may share a name",
            intent: "Branches and tags have separate names, so one name may be both.",
            set_up_script: &[
                "SELECT dolt_branch('frombr');",
                "SELECT dolt_tag('fromtag');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('frombr');",
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
                    query: "SELECT dolt_branch('fromtag');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM dolt_branches WHERE name IN ('frombr', 'fromtag')), (SELECT count(*) FROM dolt_tags WHERE tag_name IN ('frombr', 'fromtag'));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
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
fn test_dolt_user_priv_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_purge_dropped_databases() privilege checking",
            intent: "Only a superuser may purge dropped databases.",
            set_up_script: &[
                "CREATE DATABASE mydb2;",
                "DROP DATABASE mydb2;",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_purge_dropped_databases();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function dolt_purge_dropped_databases", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::Differs("Dolt's message names the missing SUPER privilege; Postgres refuses EXECUTE on a function only superusers may run, as its own admin functions")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION dolt_purge_dropped_databases TO tester;",
                    expected: Expected::Tag("GRANT"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's GRANT SUPER ON *.* is GRANT EXECUTE on the function in Postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_purge_dropped_databases();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_purge_dropped_databases", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_purge_dropped_databases();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_purge_dropped_databases", INT8)],
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
            name: "table function privilege checking",
            intent: "Dolt's history functions need SELECT on the table they name, or on every table when they name none.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "CREATE TABLE test2 (pk BIGINT PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating tables test and test2');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting into test');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~..main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~..main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~..main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main~..main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON test TO tester;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff('main~', 'main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff('main~..main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~', 'main', 'test2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~..main', 'test2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~', 'main', 'test2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~...main', 'test2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~', 'main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~...main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~', 'main', 'test2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~...main', 'test2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~', 'main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~...main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main~', 'main', 'test2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main~...main', 'test2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main~', 'main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main~...main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test2", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON test FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~..main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    dolt: Some(Dolt::Differs("Postgres grants on the tables of a schema where MySQL grants on a database or the whole server")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff('main~', 'main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff('main~..main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_stat('main~', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_stat('main~...main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('main~', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('main~...main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_patch('main~', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_patch('main~...main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log('main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON ALL TABLES IN SCHEMA public FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    dolt: Some(Dolt::Differs("Postgres grants on the tables of a schema where MySQL grants on a database or the whole server")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~...main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD^','HEAD','test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD^','HEAD');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    dolt: Some(Dolt::Differs("Postgres grants on the tables of a schema where MySQL grants on a database or the whole server")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff('main~', 'main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff('main~...main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALL ON ALL TABLES IN SCHEMA public FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    dolt: Some(Dolt::Differs("Postgres grants on the tables of a schema where MySQL grants on a database or the whole server")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~', 'main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main~..main', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_branch_status_table_function_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "test dolt_branch_status(...)",
            intent: "dolt_branch_status counts the commits each branch is ahead and behind the first, refusing calls without arguments.",
            set_up_script: &[
                "SELECT dolt_branch('b1');",
                "SELECT dolt_commit('-m', 'main', '--allow-empty');",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT dolt_commit('-m', 'b1', '--allow-empty');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b2');",
                "SELECT dolt_commit('-m', 'b2', '--allow-empty');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT dolt_branch('b3');",
                "SELECT * FROM dolt_checkout('b3');",
                "SELECT dolt_commit('-m', 'b3', '--allow-empty');",
                "SELECT dolt_branch('b4');",
                "SELECT * FROM dolt_checkout('b4');",
                "SELECT dolt_commit('-m', 'b4', '--allow-empty');",
                "SELECT dolt_branch('b5');",
                "SELECT * FROM dolt_checkout('b5');",
                "SELECT dolt_commit('-m', 'b5', '--allow-empty');",
                "SELECT dolt_tag('t1', 'b1');",
                "SELECT dolt_tag('t2', 'b2');",
                "SELECT dolt_tag('t3', 'b3');",
                "SELECT dolt_tag('t4', 'b4');",
                "SELECT dolt_tag('t5', 'b5');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "string is not a valid branch or hash", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'non-existent-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: non-existent-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_branch_status() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'main', 'b1', 'b2', 'b3', 'b4', 'b5');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("main"), T("0"), T("0")],
                            &[T("b1"), T("1"), T("1")],
                            &[T("b2"), T("2"), T("1")],
                            &[T("b3"), T("1"), T("0")],
                            &[T("b4"), T("2"), T("0")],
                            &[T("b5"), T("3"), T("0")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 't1', 't2', 't3', 't4', 't5');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("t1"), T("1"), T("1")],
                            &[T("t2"), T("2"), T("1")],
                            &[T("t3"), T("1"), T("0")],
                            &[T("t4"), T("2"), T("0")],
                            &[T("t5"), T("3"), T("0")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('b2', 'b5');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b5"), T("4"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'b5', 'HEAD', 'HEAD~1', 'HEAD~2');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b5"), T("3"), T("0")],
                            &[T("HEAD"), T("3"), T("0")],
                            &[T("HEAD~1"), T("2"), T("0")],
                            &[T("HEAD~2"), T("1"), T("0")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commits_ahead, commits_behind FROM dolt_branch_status('main', dolt_hashof('b5'));",
                    expected: Expected::Rows {
                        columns: &[Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("3"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test dolt_branch_status with merge",
            intent: "dolt_branch_status counts the commits a branch is ahead and behind, before and after a merge.",
            set_up_script: &[
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_commit('-m', 'm1', '--allow-empty');",
                "SELECT dolt_commit('-m', 'm2', '--allow-empty');",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT dolt_commit('-m', 'b1c1', '--allow-empty');",
                "SELECT dolt_commit('-m', 'b1c2', '--allow-empty');",
                "SELECT * FROM dolt_checkout('b2');",
                "SELECT dolt_commit('-m', 'b2c1', '--allow-empty');",
                "SELECT dolt_commit('-m', 'b2c2', '--allow-empty');",
                "SELECT dolt_commit('-m', 'b2c3', '--allow-empty');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'b1', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b1"), T("2"), T("2")],
                            &[T("b2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('b1', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Merge branch 'b1' into b2")],
                            &[T("b2c3")],
                            &[T("b2c2")],
                            &[T("b1c2")],
                            &[T("b2c1")],
                            &[T("b1c1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'b1', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b1"), T("2"), T("2")],
                            &[T("b2"), T("6"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('b1', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b2"), T("4"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test dolt_branch_status(...)",
            intent: "dolt_branch_status counts commits ahead and behind across tags, ancestors, and revision expressions.",
            set_up_script: &[
                "SELECT dolt_tag('anc', 'HEAD');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_commit('-m', 'C1', '--allow-empty');",
                "SELECT dolt_commit('-m', 'C2', '--allow-empty');",
                "SELECT dolt_commit('-m', 'C3', '--allow-empty');",
                "SELECT dolt_tag('t1', 'HEAD');",
                "SELECT dolt_commit('-m', 'C4', '--allow-empty');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_commit('-m', 'C5', '--allow-empty');",
                "SELECT dolt_commit('-m', 'C6', '--allow-empty');",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT dolt_commit('-m', 'C7', '--allow-empty');",
                "SELECT * FROM dolt_merge('t1', '-m', 'M1');",
                "SELECT dolt_tag('t2', 'HEAD');",
                "SELECT dolt_commit('-m', 'C8', '--allow-empty');",
                "SELECT * FROM dolt_checkout('b2');",
                "SELECT * FROM dolt_merge('b1', '-m', 'M2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b2"), T("4"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b1"), T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('anc', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("b2"), T("8"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('b2', 'anc');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("anc"), T("0"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('t1', 'anc', 't2','t2~1', 'main', 'b1', 'b2', 'b2^1', 'b2^2' );",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("anc"), T("0"), T("3")],
                            &[T("t2"), T("2"), T("0")],
                            &[T("t2~1"), T("1"), T("3")],
                            &[T("main"), T("3"), T("0")],
                            &[T("b1"), T("3"), T("0")],
                            &[T("b2"), T("5"), T("0")],
                            &[T("b2^1"), T("1"), T("0")],
                            &[T("b2^2"), T("3"), T("0")],
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
fn test_broken_history_system_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_history table with AS OF",
            intent: "dolt_log AS OF a revision lists the log of that revision.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 varchar(20));",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'creating table t');",
                "INSERT INTO t VALUES (1, 2, '3'), (4, 5, '6');",
                "SELECT dolt_commit('-am', 'added values');",
                "INSERT INTO t VALUES (11, 22, '3'), (44, 55, '6');",
                "SELECT dolt_commit('-am', 'added values again');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log AS OF 'head^';",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added values")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Bug("Dolt's expectation is right, but Dolt main fails it and keeps the script among its BrokenHistorySystemTableScriptTests. Dolt's harness starts from its checkpoint commit, where a Doltgrust database starts with its initial commit")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_checkout_read_only_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt checkout -b returns an error for read-only databases",
            intent: "dolt_checkout -b refuses to make a branch in a read-only database.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newBranch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to create new branch in a read-only database", ..E }),
                    ..A
                },
            ],
            skip: Some("2f: read-only databases (Dolt's harness makes the database read-only; the database access mode of STORAGE_DESIGN Part V)"),
            ..S
        },
        ScriptTest {
            name: "dolt checkout -B returns an error for read-only databases",
            intent: "dolt_checkout -B refuses to make a branch in a read-only database.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-B', 'newBranch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to create new branch in a read-only database", ..E }),
                    ..A
                },
            ],
            skip: Some("2f: read-only databases (Dolt's harness makes the database read-only; the database access mode of STORAGE_DESIGN Part V)"),
            ..S
        },
    ]);
}

#[test]
fn test_dolt_reflog_test_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_reflog: error cases",
            intent: "dolt_reflog refuses extra arguments and arguments that are not strings.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_reflog('foo', 'bar');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: dolt_reflog has too many positional arguments. Expected at most 1, found 2: ['foo' 'bar']", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_reflog(NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "argument (<nil>) is not a string value, but a <nil>", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_reflog(-100);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_reflog(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("Postgres resolves a function by its arguments' types, so an integer finds no dolt_reflog")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reflog: basic cases with no arguments",
            intent: "dolt_reflog with no argument lists every ref's moves, newest first.",
            set_up_script: &[
                "CREATE TABLE t1(pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t1');",
                "INSERT INTO t1 VALUES(1);",
                "SELECT dolt_commit('-Am', 'inserting row 1');",
                "SELECT dolt_tag('tag1');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t1 VALUES(2);",
                "SELECT dolt_commit('-Am', 'inserting row 2');",
                "INSERT INTO t1 VALUES(3);",
                "SELECT dolt_commit('-Am', 'inserting row 3');",
                "SELECT dolt_tag('-d', 'tag1');",
                "SELECT dolt_tag('tag1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/tags/tag1"), Any, T("inserting row 3")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 3")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 2")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 1")],
                            &[T("refs/tags/tag1"), Any, T("inserting row 1")],
                            &[T("refs/heads/main"), Any, T("inserting row 1")],
                            &[T("refs/heads/main"), Any, T("creating table t1")],
                            &[T("refs/heads/main"), Any, T("Initialize data repository")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reflog: basic cases with a ref argument",
            intent: "dolt_reflog of a branch or tag, named in full or short and in any case, lists its moves, and an unknown ref lists nothing.",
            set_up_script: &[
                "CREATE TABLE t1(pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t1');",
                "INSERT INTO t1 VALUES(1);",
                "SELECT dolt_commit('-Am', 'inserting row 1');",
                "SELECT dolt_tag('tag1');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t1 VALUES(2);",
                "SELECT dolt_commit('-Am', 'inserting row 2');",
                "INSERT INTO t1 VALUES(3);",
                "SELECT dolt_commit('-Am', 'inserting row 3');",
                "SELECT dolt_tag('-d', 'tag1');",
                "SELECT dolt_tag('tag1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_reflog('doesNotExist');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("ref_timestamp", TIMESTAMPTZ), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('refs/heads/main');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), Any, T("inserting row 1")],
                            &[T("refs/heads/main"), Any, T("creating table t1")],
                            &[T("refs/heads/main"), Any, T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('reFS/Heads/MaIn');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), Any, T("inserting row 1")],
                            &[T("refs/heads/main"), Any, T("creating table t1")],
                            &[T("refs/heads/main"), Any, T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('main');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), Any, T("inserting row 1")],
                            &[T("refs/heads/main"), Any, T("creating table t1")],
                            &[T("refs/heads/main"), Any, T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('MaIN');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), Any, T("inserting row 1")],
                            &[T("refs/heads/main"), Any, T("creating table t1")],
                            &[T("refs/heads/main"), Any, T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('refs/heads/branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/branch1"), Any, T("inserting row 3")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 2")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/branch1"), Any, T("inserting row 3")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 2")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('refs/tags/tag1');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/tags/tag1"), Any, T("inserting row 3")],
                            &[T("refs/tags/tag1"), Any, T("inserting row 1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('Refs/TAGs/taG1');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/tags/tag1"), Any, T("inserting row 3")],
                            &[T("refs/tags/tag1"), Any, T("inserting row 1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('tag1');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/tags/tag1"), Any, T("inserting row 3")],
                            &[T("refs/tags/tag1"), Any, T("inserting row 1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('tAG1');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/tags/tag1"), Any, T("inserting row 3")],
                            &[T("refs/tags/tag1"), Any, T("inserting row 1")],
                        ],
                        tag: "SELECT 2",
                    },
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
                    query: "SELECT dolt_branch('-D', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/branch1"), Any, T("inserting row 3")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 2")],
                            &[T("refs/heads/branch1"), Any, T("inserting row 1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('-d', 'tag1');",
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
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('tag1');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/tags/tag1"), Any, T("inserting row 3")],
                            &[T("refs/tags/tag1"), Any, T("inserting row 1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reflog: garbage collection with no newgen data",
            intent: "Garbage collection keeps the reflog when nothing new was written.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('main');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), Any, T("Initialize data repository")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('main');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "session1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reflog: garbage collection with newgen data",
            intent: "Garbage collection keeps the reflog after new writes.",
            set_up_script: &[
                "CREATE TABLE t1(pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t1');",
                "INSERT INTO t1 VALUES(1);",
                "SELECT dolt_commit('-Am', 'inserting row 1');",
                "SELECT dolt_tag('tag1');",
                "INSERT INTO t1 VALUES(2);",
                "SELECT dolt_commit('-Am', 'inserting row 2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('main');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), Any, T("inserting row 2")],
                            &[T("refs/heads/main"), Any, T("inserting row 1")],
                            &[T("refs/heads/main"), Any, T("creating table t1")],
                            &[T("refs/heads/main"), Any, T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_hash, commit_message FROM dolt_reflog('main');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "session1",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_remote_test_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt-remote: SQL add remotes",
            intent: "dolt_remote adds a remote, which dolt_remotes lists, and refuses missing arguments and writes to dolt_remotes.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('add','origin','file://../test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, CASE WHEN CHAR_LENGTH(url) < 0 THEN NULL ELSE 'not null' END, fetch_specs, params FROM DOLT_REMOTES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("case", TEXT), Column("fetch_specs", JSONB), Column("params", JSONB)],
                        rows: &[
                            &[T("origin"), T("not null"), T("[\"refs/heads/*:refs/remotes/origin/*\"]"), T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: invalid argument, use 'dolt_remotes' system table to list remotes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('origin');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: invalid argument", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_remotes (name, url) VALUES ('origin', 'file://../test');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot insert into view \"dolt_remotes\"", detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's dolt_remotes is a view, which Postgres refuses to write as it refuses any view that isn't updatable, where Dolt calls the table read-only")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt-remote: SQL remove remotes",
            intent: "dolt_remote removes a remote from dolt_remotes.",
            set_up_script: &[
                "SELECT dolt_remote('add','origin1','file://.');",
                "SELECT dolt_remote('add','origin2','aws://[dynamo_db_table:s3_bucket]/repo_name');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, CASE WHEN CHAR_LENGTH(url) < 0 THEN NULL ELSE 'not null' END, fetch_specs, params FROM DOLT_REMOTES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("case", TEXT), Column("fetch_specs", JSONB), Column("params", JSONB)],
                        rows: &[
                            &[T("origin1"), T("not null"), T("[\"refs/heads/*:refs/remotes/origin1/*\"]"), T("{}")],
                            &[T("origin2"), T("not null"), T("[\"refs/heads/*:refs/remotes/origin2/*\"]"), T("{}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('remove','origin2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, CASE WHEN CHAR_LENGTH(url) < 0 THEN NULL ELSE 'not null' END, fetch_specs, params FROM DOLT_REMOTES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("case", TEXT), Column("fetch_specs", JSONB), Column("params", JSONB)],
                        rows: &[
                            &[T("origin1"), T("not null"), T("[\"refs/heads/*:refs/remotes/origin1/*\"]"), T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_remotes WHERE name = 'origin1';",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot delete from view \"dolt_remotes\"", detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's dolt_remotes is a view, which Postgres refuses to write as it refuses any view that isn't updatable, where Dolt calls the table read-only")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt-remote: multi-repo test",
            intent: "A remote added in one database is not a remote of another.",
            set_up_script: &[
                "CREATE DATABASE one;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('add','test01','file:///foo');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "one",
                    database: "one",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE one is a connection to one, since a Postgres connection's database never changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_remotes WHERE name='test01';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "one",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_remotes WHERE name='test01';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE mydb returns to the script's database, which the default connection uses")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_system_variables() {
    run_scripts(&[
        ScriptTest {
            name: "DOLT_SHOW_SYSTEM_TABLES",
            intent: "With dolt_show_system_tables on, the table listings show Dolt's system tables.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SET dolt_show_system_tables = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("dolt_commit_diff_test")],
                            &[T("dolt_conflicts_test")],
                            &[T("dolt_constraint_violations_test")],
                            &[T("dolt_diff_test")],
                            &[T("dolt_history_test")],
                            &[T("dolt_workspace_test")],
                            &[T("test")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust keeps Dolt's database-wide system tables in pg_catalog, as Postgres keeps its system tables, so the schema's list shows only the per-table ones")),
                    skip: Some("information_schema.tables lists the per-table system tables differently, since Doltgrust's are views the catalog synthesizes (Appendix C, per-table system tables in listings)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'pg_catalog' AND table_name IN ('dolt_backups', 'dolt_branch_activity', 'dolt_branches', 'dolt_commit_ancestors', 'dolt_commits', 'dolt_conflicts', 'dolt_constraint_violations', 'dolt_help', 'dolt_log', 'dolt_remote_branches', 'dolt_remotes', 'dolt_stashes', 'dolt_status', 'dolt_status_ignored') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("dolt_backups")],
                            &[T("dolt_branch_activity")],
                            &[T("dolt_branches")],
                            &[T("dolt_commit_ancestors")],
                            &[T("dolt_commits")],
                            &[T("dolt_conflicts")],
                            &[T("dolt_constraint_violations")],
                            &[T("dolt_help")],
                            &[T("dolt_log")],
                            &[T("dolt_remote_branches")],
                            &[T("dolt_remotes")],
                            &[T("dolt_stashes")],
                            &[T("dolt_status")],
                            &[T("dolt_status_ignored")],
                        ],
                        tag: "SELECT 14",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust keeps Dolt's database-wide system tables in pg_catalog, as Postgres keeps its system tables, so the schema's list shows only the per-table ones")),
                    skip: Some("information_schema.tables lists Dolt's database-wide system tables differently, since Doltgrust's are built-in views in pg_catalog (Appendix C, per-table system tables in listings)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_undrop_test_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt-undrop",
            intent: "A dropped database can be undropped until the dropped databases are purged, and dolt_undrop without a name lists what it could restore.",
            set_up_script: &[
                "CREATE DATABASE one;",
                "CREATE DATABASE two;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "one",
                    database: "one",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE one is a connection to one, since a Postgres connection's database never changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES(1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "one",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'creating table t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "one",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "two",
                    database: "two",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE two is a connection to two, since a Postgres connection's database never changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES(2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "two",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'creating table t2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "two",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("one")],
                            &[T("postgres")],
                            &[T("two")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE one WITH (FORCE);",
                    expected: Expected::Tag("DROP DATABASE"),
                    dolt: Some(Dolt::Differs("Postgres refuses to drop a database a session is connected to unless FORCE ends those sessions, and the connection that set the database up is still open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                            &[T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop('one');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_undrop", INT8)],
                        rows: &[
                            &[T("0")],
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
                            &[T("one")],
                            &[T("postgres")],
                            &[T("two")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "undropped",
                    database: "one",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE one is a connection to one, since a Postgres connection's database never changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two.public.t2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "undropped",
                    skip: Some("2f core: reading another database without switching (STORAGE_DESIGN §V.3) is not supported yet"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE one WITH (FORCE);",
                    expected: Expected::Tag("DROP DATABASE"),
                    dolt: Some(Dolt::Differs("Postgres refuses to drop a database a session is connected to unless FORCE ends those sessions, and the connection that set the database up is still open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no database name specified. available databases that can be undropped: one", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_purge_dropped_databases();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_purge_dropped_databases", INT8)],
                        rows: &[
                            &[T("0")],
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
                            &[T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no database name specified. there are no databases currently available to be undropped", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_json_diff_table_function_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "basic functionality with JSON literals",
            intent: "dolt_json_diff lists the paths where two JSON documents differ, with each side's value.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_json_diff('{\"a\":1}', '{\"a\":2}');",
                    expected: Expected::Rows {
                        columns: &[Column("diff_type", TEXT), Column("path", TEXT), Column("from_value", JSONB), Column("to_value", JSONB)],
                        rows: &[
                            &[T("modified"), T("$.a"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_json_diff('{}', '{\"added_key\":\"added_value\"}');",
                    expected: Expected::Rows {
                        columns: &[Column("diff_type", TEXT), Column("path", TEXT), Column("from_value", JSONB), Column("to_value", JSONB)],
                        rows: &[
                            &[T("added"), T("$.added_key"), Null, T("\"added_value\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_json_diff('{\"removed_key\":true}', '{}');",
                    expected: Expected::Rows {
                        columns: &[Column("diff_type", TEXT), Column("path", TEXT), Column("from_value", JSONB), Column("to_value", JSONB)],
                        rows: &[
                            &[T("removed"), T("$.removed_key"), T("true"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_json_diff('{\"a\":null}', '{\"b\":null}');",
                    expected: Expected::Rows {
                        columns: &[Column("diff_type", TEXT), Column("path", TEXT), Column("from_value", JSONB), Column("to_value", JSONB)],
                        rows: &[
                            &[T("removed"), T("$.a"), T("null"), Null],
                            &[T("added"), T("$.b"), Null, T("null")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_json_diff('{\"a\": [0, 1, 2]}', '{\"a\": [0, 1, 3]}');",
                    expected: Expected::Rows {
                        columns: &[Column("diff_type", TEXT), Column("path", TEXT), Column("from_value", JSONB), Column("to_value", JSONB)],
                        rows: &[
                            &[T("modified"), T("$.a[2]"), T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_json_diff('[0, 1, 2]', '[0, 1, 3]');",
                    expected: Expected::Rows {
                        columns: &[Column("diff_type", TEXT), Column("path", TEXT), Column("from_value", JSONB), Column("to_value", JSONB)],
                        rows: &[
                            &[T("modified"), T("$[2]"), T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_json_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
            ..S
        },
        ScriptTest {
            name: "lateral join with small json objects retrieved from tables",
            intent: "dolt_json_diff of each row's two JSON columns works in a lateral join.",
            set_up_script: &[
                "CREATE TABLE test_table(pk int PRIMARY KEY, from_json json, to_json json);",
                "INSERT INTO test_table VALUES (0, '{\"a\":1}', '{\"a\":2}');",
                "INSERT INTO test_table VALUES (1, '{\"b\":3}', '{\"c\":3}');",
                "INSERT INTO test_table VALUES (2, '[0, 1, 2]', '[0, 1, 3]');",
                "INSERT INTO test_table VALUES (3, '{\"a\": [0, 1, 2]}', '{\"a\": [0, 1, 3]}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_json_diff((SELECT from_json FROM test_table WHERE pk = 0), (SELECT to_json FROM test_table WHERE pk = 0));",
                    expected: Expected::Rows {
                        columns: &[Column("diff_type", TEXT), Column("path", TEXT), Column("from_value", JSONB), Column("to_value", JSONB)],
                        rows: &[
                            &[T("modified"), T("$.a"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_json, to_json, diff_type, path, from_value, to_value FROM test_table JOIN LATERAL (SELECT * FROM dolt_json_diff(from_json, to_json)) sq;",
                    expected: Expected::Rows {
                        columns: &[Column("from_json", JSON), Column("to_json", JSON), Column("diff_type", TEXT), Column("path", TEXT), Column("from_value", JSONB), Column("to_value", JSONB)],
                        rows: &[
                            &[T("{\"a\":1}"), T("{\"a\":2}"), T("modified"), T("$.a"), T("1"), T("2")],
                            &[T("{\"b\":3}"), T("{\"c\":3}"), T("removed"), T("$.b"), T("3"), Null],
                            &[T("{\"b\":3}"), T("{\"c\":3}"), T("added"), T("$.c"), Null, T("3")],
                            &[T("[0, 1, 2]"), T("[0, 1, 3]"), T("modified"), T("$[2]"), T("2"), T("3")],
                            &[T("{\"a\": [0, 1, 2]}"), T("{\"a\": [0, 1, 3]}"), T("modified"), T("$.a[2]"), T("2"), T("3")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("Postgres' json keeps a document's text as written, where Dolt's JSON prints it normalized")),
                    ..A
                },
            ],
            skip: Some("dolt_json_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
            ..S
        },
    ]);
}

#[test]
fn test_json_value_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "create JSON table",
            intent: "A table with a JSON column can be created and committed.",
            set_up_script: &[
                "CREATE TABLE js (pk int PRIMARY KEY, js json);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM js;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSON)],
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
            name: "insert into a JSON table",
            intent: "Inserted JSON documents read back.",
            set_up_script: &[
                "CREATE TABLE js (pk int PRIMARY KEY, js json);",
                "INSERT INTO js VALUES (1, '{\"a\":1}'), (2, '{\"b\":2}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM js;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSON)],
                        rows: &[
                            &[T("1"), T("{\"a\":1}")],
                            &[T("2"), T("{\"b\":2}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Postgres' json keeps a document's text as written, where Dolt's JSON prints it normalized")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update a JSON table",
            intent: "Updated JSON documents read back.",
            set_up_script: &[
                "CREATE TABLE js (pk int PRIMARY KEY, js json);",
                "INSERT INTO js VALUES (1, '{\"a\":1}'), (2, '{\"b\":2}');",
                "UPDATE js SET js = '{\"c\":3}' WHERE pk = 2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM js;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSON)],
                        rows: &[
                            &[T("1"), T("{\"a\":1}")],
                            &[T("2"), T("{\"c\":3}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Postgres' json keeps a document's text as written, where Dolt's JSON prints it normalized")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete from a JSON table",
            intent: "Deleting a row with a JSON document leaves the others.",
            set_up_script: &[
                "CREATE TABLE js (pk int PRIMARY KEY, js json);",
                "INSERT INTO js VALUES (1, '{\"a\":1}'), (2, '{\"b\":2}');",
                "DELETE FROM js WHERE pk = 2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM js;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSON)],
                        rows: &[
                            &[T("1"), T("{\"a\":1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres' json keeps a document's text as written, where Dolt's JSON prints it normalized")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge a JSON table",
            intent: "Merging branches that changed different rows of a JSON table keeps both changes.",
            set_up_script: &[
                "CREATE TABLE js (pk int PRIMARY KEY, js json);",
                "INSERT INTO js VALUES (1, '{\"a\":1}'), (2, '{\"b\":2}');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added a JSON table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "UPDATE js SET js = '{\"b\":22}' WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'update row pk = 2');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE js SET js = '{\"a\":11}' WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'update row pk = 1');",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM js;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSON)],
                        rows: &[
                            &[T("1"), T("{\"a\": 11}")],
                            &[T("2"), T("{\"b\": 22}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("json keeps the text it was given, so the rows show {\"a\":11} as written, where Dolt's MySQL JSON prints {\"a\": 11}"),
            ..S
        },
    ]);
}

#[test]
fn test_dolt_gc() {
    run_scripts(&[
        ScriptTest {
            name: "base case: gc",
            intent: "dolt_gc refuses positional arguments, collects garbage shallowly or fully, and reports when nothing changed since the last collection.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'create table');",
                "DO $$ BEGIN FOR i IN 0..249 LOOP INSERT INTO t VALUES (i); PERFORM dolt_commit('-am', format('added pk %s', i)); END LOOP; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_gc(NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: gc does not take positional arguments, but found 1: ", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc('bad', '--shallow');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: gc does not take positional arguments, but found 1: bad", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc('--shallow');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no changes since last gc", ..E }),
                    ..A
                },
            ],
            skip: Some("dolt_gc succeeds, where the Doltgres expectation is its refusal (22023)"),
            ..S
        },
    ]);
}

#[test]
fn test_json_adaptive_encoding_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "json adaptive: small document stored inline",
            intent: "Small JSON documents read back.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"a\":1}'), (2, '{\"b\":2}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk, j FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("j", JSON)],
                        rows: &[
                            &[T("1"), T("{\"a\":1}")],
                            &[T("2"), T("{\"b\":2}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Postgres' json keeps a document's text as written, where Dolt's JSON prints it normalized")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: json_extract on small document",
            intent: "json_extract reads fields of a small document.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"name\":\"alice\",\"age\":30}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT jsonb_path_query_first((j)::jsonb, ('$.name')::jsonpath) FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query_first", JSONB)],
                        rows: &[
                            &[T("\"alice\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT jsonb_path_query_first((j)::jsonb, ('$.age')::jsonpath) FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query_first", JSONB)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: update and re-read",
            intent: "A JSON document updated in place reads back with the change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"x\":1}');",
                "UPDATE t SET j = jsonb_set((j)::jsonb, '{x}', to_jsonb(99)) WHERE pk = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT j FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("j", JSON)],
                        rows: &[
                            &[T("{\"x\": 99}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: null json values",
            intent: "A SQL NULL and a JSON null stay distinct.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, NULL), (2, 'null');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk, j FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("j", JSON)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("null")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: large document stored out-of-band",
            intent: "A large JSON document reads back whole, field by field.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, (SELECT json_object_agg(format('key_%s', lpad(i::text, 2, '0')), repeat('v', 50)) FROM generate_series(0, 49) i));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT jsonb_path_query_first((j)::jsonb, ('$.key_00')::jsonpath) FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query_first", JSONB)],
                        rows: &[
                            &[T("\"vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's harness builds the document in Go, where the setup builds it with json_object_agg")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (CASE jsonb_typeof((j)::jsonb) WHEN 'object' THEN (SELECT count(*) FROM jsonb_object_keys((j)::jsonb)) WHEN 'array' THEN jsonb_array_length((j)::jsonb) ELSE 1 END) FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("case", INT8)],
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
            name: "json adaptive: delete row with json column",
            intent: "Deleting a row with a JSON document leaves the others.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"a\":1}'), (2, '{\"b\":2}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, j FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("j", JSON)],
                        rows: &[
                            &[T("2"), T("{\"b\":2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres' json keeps a document's text as written, where Dolt's JSON prints it normalized")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: json array round-trip",
            intent: "JSON arrays read back.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '[1,2,3]'), (2, '[\"a\",\"b\",\"c\"]');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk, j FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("j", JSON)],
                        rows: &[
                            &[T("1"), T("[1,2,3]")],
                            &[T("2"), T("[\"a\",\"b\",\"c\"]")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Postgres' json keeps a document's text as written, where Dolt's JSON prints it normalized")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT jsonb_path_query_first((j)::jsonb, ('$[1]')::jsonpath) FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query_first", JSONB)],
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
            name: "json adaptive: dolt commit and read-back",
            intent: "JSON documents read back after a commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"committed\":true}');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'add json row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT j FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("j", JSON)],
                        rows: &[
                            &[T("{\"committed\":true}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres' json keeps a document's text as written, where Dolt's JSON prints it normalized")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: filter by json_extract",
            intent: "Rows filter on a field of their JSON document.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"status\":\"active\"}'), (2, '{\"status\":\"inactive\"}'), (3, '{\"status\":\"active\"}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE jsonb_path_query_first((j)::jsonb, ('$.status')::jsonpath) = 'active' ORDER BY pk;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type json", detail: "Token \"active\" is invalid.", position: 85, ..E }),
                    dolt: Some(Dolt::Differs("Postgres reads 'active' as a JSON document to compare with jsonb, which fails, where MySQL compares the JSON string with the text")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: json_set creates new field",
            intent: "json_set adds a field to a document.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"a\":1}');",
                "UPDATE t SET j = jsonb_set((j)::jsonb, '{b}', to_jsonb(2)) WHERE pk = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT j FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("j", JSON)],
                        rows: &[
                            &[T("{\"a\": 1, \"b\": 2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: nested json object",
            intent: "Nested JSON objects read back, and their inner fields extract.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"outer\":{\"inner\":{\"deep\":42}}}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT jsonb_path_query_first((j)::jsonb, ('$.outer.inner.deep')::jsonpath) FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query_first", JSONB)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: large value with control characters round-trips",
            intent: "A large string with control characters round-trips through a JSON document.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, j json, txt text);",
                "INSERT INTO t VALUES (665, json_build_object('d', repeat(chr(11), 665)), repeat(chr(11), 665));",
                "INSERT INTO t VALUES (666, json_build_object('d', repeat(chr(11), 666)), repeat(chr(11), 666));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE txt IS NULL;",
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
                    query: "SELECT char_length(txt) FROM t WHERE id = 666;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("666")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT char_length(((jsonb_path_query_first((j)::jsonb, ('$.d')::jsonpath))::jsonb #>> '{}')) FROM t WHERE id = 665;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("665")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT char_length(((jsonb_path_query_first((j)::jsonb, ('$.d')::jsonpath))::jsonb #>> '{}')) FROM t WHERE id = 666;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("666")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: json_valid on large longtext without a primary key",
            intent: "json_valid reads large text in a table with and without a primary key.",
            set_up_script: &[
                "CREATE TABLE nopk (txt text);",
                "CREATE TABLE withpk (id int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, txt text);",
                "INSERT INTO nopk VALUES (json_build_object('d', repeat('x', 2032)));",
                "INSERT INTO nopk VALUES (json_build_object('d', repeat('x', 2030)));",
                "INSERT INTO withpk (txt) VALUES (json_build_object('d', repeat('x', 2032)));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ((json_build_object('d', repeat('x', 2032))) IS JSON);",
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
                    query: "SELECT ((txt) IS JSON) FROM withpk;",
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
                    query: "SELECT ((txt) IS JSON) FROM nopk WHERE length(txt) < 2040;",
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
                    query: "SELECT ((txt) IS JSON) FROM nopk WHERE length(txt) >= 2040;",
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
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "json adaptive: html and multibyte characters round-trip out-of-band",
            intent: "Large strings with HTML and multibyte characters round-trip through JSON documents.",
            set_up_script: &[
                "CREATE TABLE h (id int PRIMARY KEY, j json);",
                "INSERT INTO h VALUES (1, json_build_object('d', repeat('<>&', 2000)));",
                "INSERT INTO h VALUES (2, json_build_object('d', repeat(convert('😀' USING utf8mb4), 2000)));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT char_length(CAST(j AS text)) FROM h WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("6009")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT char_length(((jsonb_path_query_first((j)::jsonb, ('$.d')::jsonpath))::jsonb #>> '{}')) FROM h WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("6000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT char_length(CAST(j AS text)) FROM h WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("2009")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT char_length(((jsonb_path_query_first((j)::jsonb, ('$.d')::jsonpath))::jsonb #>> '{}')) FROM h WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("2000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "json adaptive: json functions read large keyless longtext",
            intent: "JSON functions read large JSON text in a table without a primary key.",
            set_up_script: &[
                "CREATE TABLE kt (txt text);",
                "INSERT INTO kt VALUES (json_build_object('a', repeat('x', 3000), 'b', 2));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT (CASE jsonb_typeof((txt)::jsonb) WHEN 'object' THEN (SELECT count(*) FROM jsonb_object_keys((txt)::jsonb)) WHEN 'array' THEN jsonb_array_length((txt)::jsonb) ELSE 1 END) FROM kt;",
                    expected: Expected::Rows {
                        columns: &[Column("case", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT upper(jsonb_typeof((txt)::jsonb)) FROM kt;",
                    expected: Expected::Rows {
                        columns: &[Column("upper", TEXT)],
                        rows: &[
                            &[T("OBJECT")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (CASE jsonb_typeof(((SELECT jsonb_agg(k) FROM jsonb_object_keys((txt)::jsonb) k))::jsonb) WHEN 'object' THEN (SELECT count(*) FROM jsonb_object_keys(((SELECT jsonb_agg(k) FROM jsonb_object_keys((txt)::jsonb) k))::jsonb)) WHEN 'array' THEN jsonb_array_length(((SELECT jsonb_agg(k) FROM jsonb_object_keys((txt)::jsonb) k))::jsonb) ELSE 1 END) FROM kt;",
                    expected: Expected::Rows {
                        columns: &[Column("case", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT char_length(((jsonb_path_query_first((txt)::jsonb, ('$.a')::jsonpath))::jsonb #>> '{}')) FROM kt;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("3000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json adaptive: large value with ansi escape sequences round-trips",
            intent: "A large string with ANSI escape sequences round-trips through a JSON document.",
            set_up_script: &[
                "CREATE TABLE p (id varchar(64) PRIMARY KEY, data json);",
                "INSERT INTO p (id, data) SELECT 'big', convert(concat('{\"d\":\"', repeat(concat('Line ', chr(92), 'u001b[36;1mcolored', chr(92), 'u000b', chr(92), 'n'), 200), '\"}') USING utf8mb4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT upper(jsonb_typeof((data)::jsonb)) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("upper", TEXT)],
                        rows: &[
                            &[T("OBJECT")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(CAST(data AS text)) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T("6409")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT char_length(((jsonb_path_query_first((data)::jsonb, ('$.d')::jsonpath))::jsonb #>> '{}')) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("char_length", INT4)],
                        rows: &[
                            &[T("4200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
    ]);
}

#[test]
fn test_large_json_object_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "JSON under max length limit",
            intent: "A JSON document of about 70MB is stored.",
            set_up_script: &[
                "CREATE TABLE t (j json);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t (j) SELECT concat('[', repeat('\"word\",', 10000000), '\"word\"]')::json;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's INSERT ... SET is an INSERT of a SELECT")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "JSON with large string (> 1MB)",
            intent: "A JSON document holding a 3MB string stores and reads back the whole string.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j1 json);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t (pk, j1) VALUES (1, json_build_object('large_value', repeat('0123456789abcdef0123456789abcdef', 98304)));",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's harness joins random UUIDs into the string, where the test repeats a fixed one of the same length")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, length(j1->>'large_value') FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("1"), T("3145728")],
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
