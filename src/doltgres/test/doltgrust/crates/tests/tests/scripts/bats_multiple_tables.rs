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

//! Dolt's integration-tests/bats/multiple-tables.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_multiple_tables() {
    run_scripts(&[
        ScriptTest {
            name: r#"multiple-tables: examine a multi table repo"#,
            intent: r#"A repository with two new tables lists both, and both are new tables to commit."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test1")],
                            &[T("test2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is a CLI command, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("f"), T("new table")],
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"multiple-tables: modify both tables, commit only one"#,
            intent: r#"Staging and committing one of two new tables leaves the other untracked, and checking it out removes it."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"INSERT INTO test2 VALUES (0, 1, 2, 3, 4, 5);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test1');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt add is a CLI command, which dolt_add() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("t"), T("new table")],
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'added one table');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT to_table_name, diff_type FROM dolt_diff_summary('HEAD', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff_summary()")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_checkout('test2');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt checkout is a CLI command, which dolt_checkout() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is a CLI command, so the port lists the tables from pg_tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"multiple-tables: dolt add --all, dolt add -A, and dolt add . adds all changes"#,
            intent: r#"--all, -A, and . each stage every table, and resetting the tables unstages them."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"INSERT INTO test2 VALUES (0, 1, 2, 3, 4, 5);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('--all');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt add is a CLI command, which dolt_add() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("t"), T("new table")],
                            &[T("public.test2"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('test1', 'test2');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt reset is a CLI command, which dolt_reset() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("f"), T("new table")],
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt add is a CLI command, which dolt_add() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("t"), T("new table")],
                            &[T("public.test2"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('test1', 'test2');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt reset is a CLI command, which dolt_reset() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt add is a CLI command, which dolt_add() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("t"), T("new table")],
                            &[T("public.test2"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"multiple-tables: dolt commit with -a flag adds all changes"#,
            intent: r#"-a commits the changes of every staged table, leaving nothing to commit."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test1 VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"INSERT INTO test2 VALUES (0, 1, 2, 3, 4, 5);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'Commit1');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt commit prints its message, so the port reads it from dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"multiple-tables: dolt commit with -a flag does not add new table changes"#,
            intent: r#"-a commits the changes of the tracked tables but leaves a new table out."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test1 VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"INSERT INTO test2 VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"CREATE TABLE test3 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Commit1');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test3"), T("f"), T("new table")],
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
            name: r#"multiple-tables: sql dolt commit with -a flag does not add new table changes"#,
            intent: r#"-a commits the changes of the tracked tables but leaves a new table out."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test1 VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"INSERT INTO test2 VALUES (0, 1, 2, 3, 4, 5);"#,
                r#"CREATE TABLE test3 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Commit1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test3"), T("f"), T("new table")],
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
            name: r#"multiple-tables: dolt reset . resets all tables"#,
            intent: r#"Resetting . unstages every staged table."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('--all');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("t"), T("new table")],
                            &[T("public.test2"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('.');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt reset is a CLI command, which dolt_reset() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("f"), T("new table")],
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"multiple-tables: dolt reset --hard"#,
            intent: r#"A hard reset undoes the changes of tracked tables and of staged new tables, and keeps an unstaged new table."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('--all');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("t"), T("new table")],
                            &[T("public.test2"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('.');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt reset is a CLI command, which dolt_reset() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("f"), T("new table")],
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('--all');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt add is a CLI command, which dolt_add() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'commit file1 and file2');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test1 VALUES (0, 1, 2, 3, 4, 5);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test2 VALUES (0, 1, 2, 3, 4, 5);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test3 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test4 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("f"), T("modified")],
                            &[T("public.test2"), T("f"), T("modified")],
                            &[T("public.test3"), T("f"), T("new table")],
                            &[T("public.test4"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test1', 'test2', 'test3');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt add is a CLI command, which dolt_add() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt reset is a CLI command, which dolt_reset() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test4"), T("f"), T("new table")],
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
            name: r#"multiple-tables: dolt reset --hard on new tables"#,
            intent: r#"A hard reset keeps untracked new tables and removes staged ones."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt reset is a CLI command, which dolt_reset() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("f"), T("new table")],
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test1', 'test2');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt add is a CLI command, which dolt_add() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt reset is a CLI command, which dolt_reset() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is a CLI command, so the port lists the tables from pg_tables")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
