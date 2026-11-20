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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_dtables.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_status_table_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_status detached head is read-only clean",
            intent: "dolt_status of a tag or a commit, which has no working set, is empty, and dolt_status is not found in another schema.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'empty commit');",
                "SELECT dolt_tag('tag1');",
                "SELECT set_config('test.head_hash', dolt_hashof('main'), false);",
                "CREATE FUNCTION status_by_hash() RETURNS SETOF dolt_status LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY EXECUTE format('SELECT * FROM %I.pg_catalog.dolt_status', 'postgres/' || current_setting('test.head_hash')); END; $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/tag1\".pg_catalog.dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM status_by_hash();",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM information_schema.dolt_status;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"information_schema.dolt_status\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status tests",
            intent: "dolt_status reads the working set of the session's branch, a branch the name qualifies, or the revision AS OF names, and is empty for a commit or tag.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'empty commit');",
                "SELECT set_config('test.commit1', dolt_hashof('HEAD'), false);",
                "SELECT dolt_tag('tag1');",
                "SELECT dolt_checkout('-b', 'branch1');",
                "CREATE TABLE abc (pk int);",
                "SELECT dolt_add('abc');",
                "SELECT dolt_checkout('main');",
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
            ],
            assertions: &[
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
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".pg_catalog.dolt_status;",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'tag1';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF current_setting('test.commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'head';",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status AS OF 'branch1';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.abc"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch1\".pg_catalog.dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.abc"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_history_system_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "empty table",
            intent: "dolt_history_<t> of a table with no rows in any commit has no rows.",
            set_up_script: &[
                "CREATE TABLE t (n int, c varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM DOLT_HISTORY_t;",
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
            name: "keyless table",
            intent: "dolt_history_<t> of a keyless table has each commit's rows, whatever the case of its name.",
            set_up_script: &[
                "CREATE TABLE foo1 (n int, de varchar(20));",
                "INSERT INTO foo1 VALUES (1, 'Ein'), (2, 'Zwei'), (3, 'Drei');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'inserting into foo1', '--date', '2022-08-06T12:00:00');",
                "SELECT set_config('test.Commit1', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "UPDATE foo1 SET de='Eins' WHERE n=1;",
                "SELECT dolt_commit('-am', 'updating data in foo1', '--date', '2022-08-06T12:00:01');",
                "SELECT set_config('test.Commit2', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "INSERT INTO foo1 VALUES (4, 'Vier');",
                "SELECT dolt_commit('-am', 'inserting data in foo1', '--date', '2022-08-06T12:00:02');",
                "SELECT set_config('test.Commit3', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM DOLT_HISTORY_foO1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, de FROM dolt_history_foo1 WHERE dolt_commit_hash=current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("de", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Ein")],
                            &[T("2"), T("Zwei")],
                            &[T("3"), T("Drei")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, de FROM dolt_history_Foo1 WHERE dolt_commit_hash=current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("de", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Eins")],
                            &[T("2"), T("Zwei")],
                            &[T("3"), T("Drei")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, de FROM dolt_history_foo1 WHERE dolt_commit_hash=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("de", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Eins")],
                            &[T("2"), T("Zwei")],
                            &[T("3"), T("Drei")],
                            &[T("4"), T("Vier")],
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
            name: "primary key table: basic cases",
            intent: "dolt_history_<t> has each commit's rows in the current columns, NULL in commits before a column existed.",
            set_up_script: &[
                "CREATE TABLE t1 (n int PRIMARY KEY, de varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1, 'Eins'), (2, 'Zwei'), (3, 'Drei');",
                "SELECT dolt_commit('-am', 'inserting into t1', '--date', '2022-08-06T12:00:01');",
                "SELECT set_config('test.Commit1', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "ALTER TABLE t1 ADD COLUMN fr varchar(20);",
                "INSERT INTO t1 VALUES (4, 'Vier', 'Quatre');",
                "SELECT dolt_commit('-am', 'adding column and inserting data in t1', '--date', '2022-08-06T12:00:02');",
                "SELECT set_config('test.Commit2', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "UPDATE t1 SET fr='Un' WHERE n=1;",
                "UPDATE t1 SET fr='Deux' WHERE n=2;",
                "SELECT dolt_commit('-am', 'updating data in t1', '--date', '2022-08-06T12:00:03');",
                "SELECT set_config('test.Commit3', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "UPDATE t1 SET de=concat(de, ', meine herren') WHERE n>1;",
                "SELECT dolt_commit('-am', 'be polite when you address a gentleman', '--date', '2022-08-06T12:00:04');",
                "SELECT set_config('test.Commit4', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "DELETE FROM t1 WHERE n=2;",
                "SELECT dolt_commit('-am', 'we don''t need the number 2', '--date', '2022-08-06T12:00:05');",
                "SELECT set_config('test.Commit5', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM Dolt_History_t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("18")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, de, fr FROM dolt_history_T1 WHERE dolt_commit_hash = current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("de", VARCHAR), Column("fr", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Eins"), Null],
                            &[T("2"), T("Zwei"), Null],
                            &[T("3"), T("Drei"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT de, fr FROM dolt_history_T1 WHERE dolt_commit_hash = current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("de", VARCHAR), Column("fr", VARCHAR)],
                        rows: &[
                            &[T("Eins"), Null],
                            &[T("Zwei"), Null],
                            &[T("Drei"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, de, fr FROM dolt_history_T1 WHERE dolt_commit_hash = current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("de", VARCHAR), Column("fr", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Eins"), Null],
                            &[T("2"), T("Zwei"), Null],
                            &[T("3"), T("Drei"), Null],
                            &[T("4"), T("Vier"), T("Quatre")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, de, fr FROM dolt_history_T1 WHERE dolt_commit_hash = current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("de", VARCHAR), Column("fr", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Eins"), T("Un")],
                            &[T("2"), T("Zwei"), T("Deux")],
                            &[T("3"), T("Drei"), Null],
                            &[T("4"), T("Vier"), T("Quatre")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, de, fr FROM dolt_history_T1 WHERE dolt_commit_hash = current_setting('test.Commit4');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("de", VARCHAR), Column("fr", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Eins"), T("Un")],
                            &[T("2"), T("Zwei, meine herren"), T("Deux")],
                            &[T("3"), T("Drei, meine herren"), Null],
                            &[T("4"), T("Vier, meine herren"), T("Quatre")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, de, fr FROM dolt_history_T1 WHERE dolt_commit_hash = current_setting('test.Commit5');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("de", VARCHAR), Column("fr", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Eins"), T("Un")],
                            &[T("3"), T("Drei, meine herren"), Null],
                            &[T("4"), T("Vier, meine herren"), T("Quatre")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT de, fr, dolt_commit_hash=current_setting('test.commit1'), dolt_commit_hash=current_setting('test.commit2'), dolt_commit_hash=current_setting('test.commit3'), dolt_commit_hash=current_setting('test.commit4') FROM dolt_history_T1 WHERE n=2 ORDER by dolt_commit_date;",
                    expected: Expected::Rows {
                        columns: &[Column("de", VARCHAR), Column("fr", VARCHAR), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("Zwei"), Null, T("t"), T("f"), T("f"), T("f")],
                            &[T("Zwei"), Null, T("f"), T("t"), T("f"), T("f")],
                            &[T("Zwei"), T("Deux"), T("f"), T("f"), T("t"), T("f")],
                            &[T("Zwei, meine herren"), T("Deux"), T("f"), T("f"), T("f"), T("t")],
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
            name: "index by primary key",
            intent: "Filters on dolt_history_<t>'s key and commit_hash return the rows they match in each commit.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2), (3,4);",
                "SELECT dolt_commit('-am', 'initial table');",
                "SELECT set_config('test.Commit1', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "INSERT INTO t1 VALUES (5,6), (7,8);",
                "SELECT dolt_commit('-am', 'two more rows');",
                "SELECT set_config('test.Commit2', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk, c, dolt_commit_hash = current_setting('test.Commit1'), dolt_commit_hash = current_setting('test.Commit2') FROM dolt_history_t1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("1"), T("2"), T("f"), T("t")],
                            &[T("3"), T("4"), T("f"), T("t")],
                            &[T("5"), T("6"), T("f"), T("t")],
                            &[T("7"), T("8"), T("f"), T("t")],
                            &[T("1"), T("2"), T("t"), T("f")],
                            &[T("3"), T("4"), T("t"), T("f")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("1"), T("2")],
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                            &[T("5"), T("6")],
                            &[T("7"), T("8")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 ORDER by pk, c;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("1"), T("2")],
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                            &[T("5"), T("6")],
                            &[T("7"), T("8")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 WHERE pk = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 WHERE pk = 3 AND dolt_commit_hash = current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 WHERE pk = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("explain plan shows go-mysql-server's index use; dolt_history_<t> is a view over a function scan that takes the key filter pushed into it, so this checks the rows that filter finds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 WHERE pk = 3 AND dolt_committer = 'someguy';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("explain plan shows go-mysql-server's index use; dolt_history_<t> is a view over a function scan that takes the key filter pushed into it, so this checks the rows that filter finds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding an index",
            intent: "Filters on a column a later commit indexed return its rows in every commit.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2), (3,4);",
                "SELECT dolt_commit('-am', 'initial table');",
                "SELECT set_config('test.Commit1', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "INSERT INTO t1 VALUES (5,6), (7,8);",
                "SELECT dolt_commit('-am', 'two more rows');",
                "INSERT INTO t1 VALUES (9,10), (11,12);",
                "CREATE INDEX t1_c ON t1(c);",
                "SELECT dolt_commit('-am', 'two more rows and an index');",
                "SELECT set_config('test.Commit2', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("1"), T("2")],
                            &[T("1"), T("2")],
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                            &[T("5"), T("6")],
                            &[T("5"), T("6")],
                            &[T("7"), T("8")],
                            &[T("7"), T("8")],
                            &[T("9"), T("10")],
                            &[T("11"), T("12")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 WHERE c = 4 ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 WHERE c = 10 ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("9"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 WHERE c = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("explain plan shows go-mysql-server's index use; dolt_history_<t> is a view over a function scan that takes the key filter pushed into it, so this checks the rows that filter finds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c FROM dolt_history_t1 WHERE c = 10 AND dolt_committer = 'someguy';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("explain plan shows go-mysql-server's index use; dolt_history_<t> is a view over a function scan that takes the key filter pushed into it, so this checks the rows that filter finds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary key table: non-pk column drops and adds",
            intent: "History pairs a column by its column id, so a renamed column shows its values in commits before the rename.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, '3'), (4, 5, '6');",
                "SELECT dolt_commit('-am', 'creating table t');",
                "SELECT set_config('test.Commit1', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT dolt_commit('-am', 'dropping column c2');",
                "SELECT set_config('test.Commit2', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
                "ALTER TABLE t RENAME COLUMN c1 TO c2;",
                "SELECT dolt_commit('-am', 'renaming c1 to c2');",
                "SELECT set_config('test.Commit3', ((SELECT dolt_hashof('HEAD')))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_history_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM dolt_history_t;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "c1" does not exist"#, hint: "Perhaps you meant to reference the column \"dolt_history_t.c2\".", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c2 FROM dolt_history_t WHERE dolt_commit_hash=current_setting('test.Commit1') ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and Dolt pairs columns other than the key by name, so a renamed column's values from before the rename show as NULL, where columns pair by column id first, Appendix C \"a column renamed between two versions of a table\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c2 FROM dolt_history_t WHERE dolt_commit_hash=current_setting('test.Commit2') ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and Dolt pairs columns other than the key by name, so a renamed column's values from before the rename show as NULL, where columns pair by column id first, Appendix C \"a column renamed between two versions of a table\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c2 FROM dolt_history_t WHERE dolt_commit_hash=current_setting('test.Commit3') ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("4"), T("5")],
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
            name: "primary key table: non-pk column type changes",
            intent: "Values whose type the current column's can't take read as NULL, with a warning naming the column.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 varchar(20));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (1, 2, '3'), (4, 5, '6');",
                "SELECT dolt_commit('-Am', 'inserting two rows');",
                "SELECT set_config('test.Commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_commit('--allow-empty', '-m', 'empty commit');",
                "SELECT set_config('test.Commit3', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ALTER COLUMN c2 TYPE int USING c2::int;",
                "SELECT dolt_commit('-am', 'changed type of c2');",
                "SELECT set_config('test.Commit4', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_history_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "Unable to convert field c2 in historical rows because its type (character varying(20)) doesn't match current schema's type (integer)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c2 FROM dolt_history_t WHERE dolt_commit_hash=current_setting('test.Commit2') ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "Unable to convert field c2 in historical rows because its type (character varying(20)) doesn't match current schema's type (integer)", ..N }],
                    dolt: Some(Dolt::MySqlOnly("MySQL converts a column's values when its type changes, which Postgres does with USING")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c2 FROM dolt_history_t WHERE dolt_commit_hash=current_setting('test.Commit4') ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("4"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c1, c2 FROM dolt_history_t WHERE pk=4;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("4"), T("5"), T("6")],
                            &[T("4"), T("5"), Null],
                            &[T("4"), T("5"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "Unable to convert field c2 in historical rows because its type (character varying(20)) doesn't match current schema's type (integer)", ..N }],
                    dolt: Some(Dolt::Bug("Dolt's warning names the current type as the field's and the field's as the current one")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary key table: rename table",
            intent: "A renamed table's history has only the commits where it has the new name.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, '3'), (4, 5, '6');",
                "SELECT dolt_commit('-am', 'creating table t');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t RENAME TO t2;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'renaming table to t2');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_history_t;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"dolt_history_t\" does not exist", position: 22, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_history_T2;",
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
                    query: "SELECT pk, c1, c2 FROM dolt_history_t2 WHERE dolt_commit_hash != current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("4"), T("5"), T("6")],
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
            name: "primary key table: delete and recreate table",
            intent: "A table dropped and made again keeps the rows of the older commits that had a table of its name.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, '3'), (4, 5, '6');",
                "SELECT dolt_commit('-am', 'creating table t');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "DROP TABLE t;",
                "SELECT dolt_commit('-am', 'dropping table t');",
                "SELECT set_config('test.Commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'recreating table t');",
                "SELECT set_config('test.Commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_history_t;",
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
            name: "dolt_history table with AS OF",
            intent: "dolt_history_<t> AS OF a revision reads that revision's history.",
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
                    query: "SELECT count(*) FROM dolt_history_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_history_t AS OF 'head^';",
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
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added values again")],
                            &[T("added values")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness starts from its checkpoint commit; ours shows a new database's initial commit")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_history table with AS OF",
            intent: "dolt_log AS OF a revision lists that revision's commits.",
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
                    dolt: Some(Dolt::Setup("Dolt's harness starts from its checkpoint commit; ours shows a new database's initial commit")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_history table with enums",
            intent: "dolt_history_<t> reads text values, which stand for Dolt's enum here.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'creating table t');",
                "INSERT INTO t VALUES (1, 'foo');",
                "SELECT dolt_commit('-am', 'added values');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT c1 FROM dolt_history_t;",
                    expected: Expected::Rows {
                        columns: &[Column("c1", TEXT)],
                        rows: &[
                            &[T("foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_history table index lookup",
            intent: "A key filter on dolt_history_<t> counts the key's row in each commit that has it.",
            set_up_script: &[
                "CREATE TABLE yx (y int, x int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'creating table');",
                "INSERT INTO yx VALUES (0, 1);",
                "SELECT dolt_commit('-am', 'add data');",
                "INSERT INTO yx VALUES (2, 3);",
                "SELECT dolt_commit('-am', 'add data');",
                "INSERT INTO yx VALUES (4, 5);",
                "SELECT dolt_commit('-am', 'add data');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(x) FROM dolt_history_yx WHERE x = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
        ScriptTest {
            name: "dolt_history table filter correctness",
            intent: "A filter on commit_hash with a subquery returns that commit's rows.",
            set_up_script: &[
                "CREATE TABLE xy (x int PRIMARY KEY, y int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'creating table');",
                "INSERT INTO xy VALUES (0, 1);",
                "SELECT dolt_commit('-am', 'add data');",
                "INSERT INTO xy VALUES (2, 3);",
                "SELECT dolt_commit('-am', 'add data');",
                "INSERT INTO xy VALUES (4, 5);",
                "SELECT dolt_commit('-am', 'add data');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_history_xy WHERE dolt_commit_hash = (SELECT dolt_log.commit_hash FROM dolt_log LIMIT 1 OFFSET 1);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
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
            name: "dolt_history table primary key with join",
            intent: "dolt_history_<t> joins dolt_commits on commit_hash and sorts by its columns.",
            set_up_script: &[
                "CREATE TABLE xyz (x int, y int, z int, PRIMARY KEY(x, y));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'creating table');",
                "INSERT INTO xyz VALUES (0, 1, 100);",
                "SELECT dolt_commit('-am', 'add data');",
                "INSERT INTO xyz VALUES (2, 3, 200);",
                "SELECT dolt_commit('-am', 'add data');",
                "INSERT INTO xyz VALUES (4, 5, 300);",
                "SELECT dolt_commit('-am', 'add data');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT\n  dolt_history_xyz.x AS x,\n  dolt_history_xyz.y AS y,\n  dolt_history_xyz.z AS z,\n  dolt_commits.commit_hash AS comm\nFROM\n  dolt_history_xyz\n  LEFT JOIN\n  dolt_commits\n  ON\n  dolt_history_xyz.dolt_commit_hash = dolt_commits.commit_hash\nORDER BY\n  dolt_history_xyz.x,\n  dolt_history_xyz.y,\n  dolt_history_xyz.z;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4), Column("comm", TEXT)],
                        rows: &[
                            &[T("0"), T("1"), T("100"), Any],
                            &[T("0"), T("1"), T("100"), Any],
                            &[T("0"), T("1"), T("100"), Any],
                            &[T("2"), T("3"), T("200"), Any],
                            &[T("2"), T("3"), T("200"), Any],
                            &[T("4"), T("5"), T("300"), Any],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT\n  dolt_history_xyz.y AS y,\n  dolt_history_xyz.z AS z,\n  dolt_commits.commit_hash AS comm\nFROM\n  dolt_history_xyz\n  LEFT JOIN\n  dolt_commits\n  ON\n  dolt_history_xyz.dolt_commit_hash = dolt_commits.commit_hash\nORDER BY\n  dolt_history_xyz.y,\n  dolt_history_xyz.z;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4), Column("z", INT4), Column("comm", TEXT)],
                        rows: &[
                            &[T("1"), T("100"), Any],
                            &[T("1"), T("100"), Any],
                            &[T("1"), T("100"), Any],
                            &[T("3"), T("200"), Any],
                            &[T("3"), T("200"), Any],
                            &[T("5"), T("300"), Any],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT\n  dolt_history_xyz.z AS z,\n  dolt_commits.commit_hash AS comm\nFROM\n  dolt_history_xyz\n  LEFT JOIN\n  dolt_commits\n  ON\n  dolt_history_xyz.dolt_commit_hash = dolt_commits.commit_hash\nORDER BY\n  dolt_history_xyz.z;",
                    expected: Expected::Rows {
                        columns: &[Column("z", INT4), Column("comm", TEXT)],
                        rows: &[
                            &[T("100"), Any],
                            &[T("100"), Any],
                            &[T("100"), Any],
                            &[T("200"), Any],
                            &[T("200"), Any],
                            &[T("300"), Any],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT z\nFROM xyz\nWHERE z IN (\n  SELECT z\n  FROM dolt_history_xyz\n  LEFT JOIN dolt_commits\n  ON dolt_history_xyz.dolt_commit_hash = dolt_commits.commit_hash\n);",
                    expected: Expected::Rows {
                        columns: &[Column("z", INT4)],
                        rows: &[
                            &[T("100")],
                            &[T("200")],
                            &[T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can sort by dolt_log.commit",
            intent: "The commit tables sort by commit_hash.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'something' FROM dolt_log ORDER by commit_hash;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("something")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'something' FROM dolt_diff ORDER by commit_hash;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'something' FROM dolt_commits ORDER by commit_hash;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("something")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'something' FROM dolt_commit_ancestors ORDER by commit_hash;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("something")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_history table non-unique pk columns ignored for max1row optimization",
            intent: "A key filter on dolt_history_<t> returns the key's row from every commit, not just one.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY, message TEXT);",
                "INSERT INTO t1 (id, message) VALUES (1, 'test1');",
                "INSERT INTO t1 (id, message) VALUES (2, 'irrelevant');",
                "SELECT dolt_commit('-A', '-m', 'test commit 1');",
                "UPDATE t1 SET message='test2' WHERE id=1;",
                "SELECT dolt_commit('-a', '-m', 'test commit 2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, message FROM dolt_history_t1 WHERE id = 1 ORDER by dolt_commit_date DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("test2")],
                            &[T("1"), T("test1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, message FROM dolt_history_t1 WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("message", TEXT)],
                        rows: &[
                            &[T("2"), T("irrelevant")],
                            &[T("2"), T("irrelevant")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_commit_ancestors table with commit_hash filter ignored for max1row optimization",
            intent: "dolt_commit_ancestors has a row for each parent of a merge commit.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "CREATE TABLE t1 (id int PRIMARY KEY, message TEXT);",
                "INSERT INTO t1 (id, message) VALUES (1, 'test1');",
                "SELECT dolt_commit('-A', '-m', 'test commit 1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "UPDATE t1 SET message='test2' WHERE id=1;",
                "SELECT dolt_commit('-A', '-m', 'test commit 2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t1 (id, message) VALUES (2, 'test3');",
                "SELECT dolt_commit('-A', '-m', 'test commit 3');",
                "SELECT dolt_merge('--no-ff', 'branch2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT parent_index FROM dolt_commit_ancestors WHERE commit_hash = (SELECT dolt_hashof('HEAD'));",
                    expected: Expected::Rows {
                        columns: &[Column("parent_index", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dca.parent_index, (SELECT message FROM dolt_log WHERE commit_hash = dca.parent_hash) AS message FROM dolt_commit_ancestors dca WHERE dca.commit_hash = (SELECT dolt_hashof('HEAD')) ORDER BY dca.parent_index;",
                    expected: Expected::Rows {
                        columns: &[Column("parent_index", INT4), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("test commit 3")],
                            &[T("1"), T("test commit 2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_history_dolt_schemas committer column when unset reflects --author",
            intent: "dolt_history_dolt_schemas shows --author as the committer.",
            set_up_script: &[
                "CREATE VIEW v1 AS SELECT 1;",
                "SELECT dolt_commit('-Am', 'create view', '--author', 'Test Author <test@example.com>');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_committer FROM dolt_history_dolt_schemas WHERE name = 'v1';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("Test Author <test@example.com>")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "dolt_history_dolt_procedures committer column when unset reflects --author",
            intent: "dolt_history_dolt_procedures shows --author as the committer.",
            set_up_script: &[
                "CREATE PROCEDURE p1() SELECT 1;",
                "SELECT dolt_commit('-Am', 'create procedure', '--author', 'Test Author <test@example.com>');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_committer FROM dolt_history_dolt_procedures WHERE name = 'p1';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("Test Author <test@example.com>")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "dolt_commit_ancestors returns one row per parent for a merge commit looked up by commit_hash",
            intent: "dolt_commit_ancestors has both parents of a merge commit.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-A', '-m', 'base');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-A', '-m', 'other');",
                "SELECT set_config('test.other', (dolt_hashof('other'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-A', '-m', 'main');",
                "SELECT set_config('test.main', (dolt_hashof('main'))::text, FALSE);",
                "SELECT dolt_merge('--no-ff', '-m', 'merged', 'other');",
                "SELECT set_config('test.merge', (dolt_hashof('main'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT parent_index FROM dolt_commit_ancestors WHERE commit_hash = current_setting('test.merge');",
                    expected: Expected::Rows {
                        columns: &[Column("parent_index", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT CASE WHEN parent_hash IN (current_setting('test.main'), current_setting('test.other')) THEN 'ok' ELSE 'bad' END FROM dolt_commit_ancestors WHERE commit_hash = current_setting('test.merge');",
                    expected: Expected::Rows {
                        columns: &[Column("case", TEXT)],
                        rows: &[
                            &[T("ok")],
                            &[T("ok")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
