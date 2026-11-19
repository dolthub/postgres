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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_diff.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_diff_system_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_diff_ table to_commit and from_commit indexes are non-unique",
            intent: "A filter on dolt_diff_<t>'s to_commit finds every row the commit changed, since many rows share a commit.",
            set_up_script: &[
                "CREATE TABLE foo (id int PRIMARY KEY, v int);",
                "INSERT INTO foo VALUES (1,1),(2,1),(3,1),(4,1);",
                "SELECT dolt_commit('-Am', 'seed');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_foo;",
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
                    query: "SELECT COUNT(*) FROM dolt_diff_foo WHERE dolt_to_commit = dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW INDEX is MySQL's and dolt_diff_foo is a view without indexes; a to_commit filter, which is pushed into its scan, still finds all four rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "base case: added rows",
            intent: "dolt_diff_<t> shows a commit's inserted rows as added, with NULL from_ columns, whatever the case of its name.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_T;",
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
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') ORDER BY to_pk, to_c2, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
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
            name: "base case: modified rows",
            intent: "dolt_diff_<t> shows an updated row as modified, with its old values in the from_ columns.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "UPDATE t SET c2=0 WHERE pk=1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'modifying row'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') ORDER BY to_pk, to_c2, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("0"), T("1"), T("2"), T("3"), T("modified")],
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
            name: "base case: deleted row",
            intent: "dolt_diff_<t> shows a deleted row as removed, with NULL to_ columns.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "DELETE FROM t WHERE pk=1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'modifying row'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("2"), T("3"), T("removed")],
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
            name: "table drop and recreate with overlapping schema",
            intent: "dolt_diff_<t> stops at the commit that dropped the table, so a recreated table shows only its own rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "DROP TABLE t;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping table t'), FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (100, 200), (300, 400);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'recreating table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "due to primary key set change", message_contains: true, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("200"), Null, Null, T("added")],
                            &[T("300"), T("400"), Null, Null, T("added")],
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
            name: "column drop",
            intent: "A dropped column is left out of dolt_diff_<t>, and dropping it shows every row as modified.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c2, from_pk, from_c2 FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c2", INT4)],
                        rows: &[
                            &[T("1"), T("3"), Null, Null],
                            &[T("4"), T("6"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c2, from_pk, from_c2 FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c2", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("1"), T("3")],
                            &[T("4"), T("6"), T("4"), T("6")],
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
            name: "column drop and recreate with same type",
            intent: "A column dropped and added again under its name reads its old values by name in earlier commits.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null, T("added")],
                            &[T("3"), T("4"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("1"), T("2"), T("modified")],
                            &[T("3"), Null, T("3"), T("4"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
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
            name: "column drop, then rename column with same type to same name",
            intent: "A column renamed to a dropped column's name reads the dropped column's values in earlier commits, matching by name.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c1'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c2 TO c1;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("3"), Null, Null, T("added")],
                            &[T("4"), T("6"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and Dolt pairs columns other than the key by name, so a renamed column's values from before the rename show as NULL, where columns pair by column id first, Appendix C \"a column renamed between two versions of a table\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("3"), T("1"), T("3"), T("modified")],
                            &[T("4"), T("6"), T("4"), T("6"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and Dolt pairs columns other than the key by name, so a renamed column's values from before the rename show as NULL, where columns pair by column id first, Appendix C \"a column renamed between two versions of a table\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
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
            name: "column drop and recreate with different type that can be coerced (int -> string)",
            intent: "A column added again with a type its old values convert to reads them converted in earlier commits.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c varchar(20);",
                "INSERT INTO t VALUES (100, '101');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 're-adding column c'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", VARCHAR), Column("from_pk", INT4), Column("from_c", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null, T("added")],
                            &[T("3"), T("4"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", VARCHAR), Column("from_pk", INT4), Column("from_c", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("1"), T("2"), T("modified")],
                            &[T("3"), Null, T("3"), T("4"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", VARCHAR), Column("from_pk", INT4), Column("from_c", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
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
            name: "column drop and recreate with different type that can NOT be coerced (string -> int)",
            intent: "Old values that don't convert to a column's new type read as NULL, with a warning for each.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 'two'), (3, 'four');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 're-adding column c'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, Null, Null, T("added")],
                            &[T("3"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("1"), Null, T("modified")],
                            &[T("3"), Null, T("3"), Null, T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_t;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "columns modified to narrower types",
            intent: "Old values that don't fit a narrowed column read as NULL, with a warning for each.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(20), col2 int);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "INSERT INTO t VALUES (1, '123456789012345', 42000);",
                "SELECT dolt_commit('-am', 'inserting data');",
                "UPDATE t SET col1='1234567890', col2=13;",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(10);",
                "ALTER TABLE t ALTER COLUMN col2 TYPE smallint;",
                "SELECT dolt_commit('-am', 'narrowing types');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_col1, to_col2, dolt_to_commit, from_pk, from_col1, from_col2, dolt_from_commit, dolt_diff_type FROM dolt_diff_t ORDER by dolt_diff_type ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_col1", VARCHAR), Column("to_col2", INT2), Column("dolt_to_commit", TEXT), Column("from_pk", INT4), Column("from_col1", VARCHAR), Column("from_col2", INT2), Column("dolt_from_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, Null, Any, Null, Null, Null, Any, T("added")],
                            &[T("1"), T("1234567890"), T("13"), Any, T("1"), Null, Null, Any, T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col1' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col2' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col1' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col2' into latest column schema", ..N }],
                    dolt: Some(Dolt::MySqlOnly("tinyint is MySQL's, so col2 narrows to smallint and holds 42000 where Dolt's held 420, which still doesn't fit")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_col1, to_col2, from_col1, from_col2 FROM dolt_diff_t ORDER BY dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_col1", VARCHAR), Column("to_col2", INT2), Column("from_col1", VARCHAR), Column("from_col2", INT2)],
                        rows: &[
                            &[Null, Null, Null, Null],
                            &[T("1234567890"), T("13"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col1' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col2' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col1' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col2' into latest column schema", ..N }],
                    dolt: Some(Dolt::MySqlOnly("SHOW WARNINGS is MySQL's; Postgres sends each warning with the query that raised it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple column renames",
            intent: "A column renamed and then dropped and added again reads only the values its name held in each commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c1 TO c2;",
                "INSERT INTO t VALUES (3, 4);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'renaming c1 to c2'), FALSE);",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'dropping column c2'), FALSE);",
                "ALTER TABLE t ADD COLUMN c2 int;",
                "INSERT INTO t VALUES (100, '101');",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'recreating column c2'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c2, from_pk, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c2, from_pk, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), T("4"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c2, from_pk, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("1"), T("2"), T("modified")],
                            &[T("3"), Null, T("3"), T("4"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c2, from_pk, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit4') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
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
            name: "primary key change",
            intent: "dolt_diff_<t> warns at a commit that changed the primary key and shows no commits before it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "INSERT INTO t VALUES (5, 6);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping primary key'), FALSE);",
                "ALTER TABLE t ADD PRIMARY KEY (c1);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'adding primary key'), FALSE);",
                "INSERT INTO t VALUES (7, 8);",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'adding more data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_t;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_DIFF_t WHERE dolt_to_commit=current_setting('test.Commit4');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("7"), T("8"), Null, Null, T("added")],
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
            name: "altered keyless table add pk",
            intent: "Adding a primary key to a keyless table shows its rows as removed and added again, with a warning.",
            set_up_script: &[
                "CREATE TABLE tbl (i int, j int);",
                "INSERT INTO tbl VALUES (42, 23);",
                "SELECT dolt_commit('-Am', 'commit1');",
                "ALTER TABLE tbl ADD PRIMARY KEY(i);",
                "SELECT dolt_commit('-am', 'commit2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_i,to_j,from_i,from_j,dolt_diff_type  FROM dolt_diff_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("to_i", INT4), Column("to_j", INT4), Column("from_i", INT4), Column("from_j", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("42"), T("23"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table with commit column should maintain its data in diff",
            intent: "A user column named like a commit column doesn't hide dolt_diff_<t>'s own to_commit and from_commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, commit_msg varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table t');",
                "INSERT INTO t VALUES (1, 'hi');",
                "SELECT dolt_commit('-am', 'insert data');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, char_length(dolt_to_commit), from_pk, char_length(dolt_from_commit), dolt_diff_type FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("char_length", INT4), Column("from_pk", INT4), Column("char_length", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("52"), Null, T("52"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust's hashes are its own format's addresses, so they never equal Dolt's, Appendix C \"commit and table hashes\"; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "selecting to_pk columns",
            intent: "Filters on dolt_diff_<t>'s to_ key columns, pushed into its scan or not, return the same rows as filtering afterwards.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'first commit'), FALSE);",
                "INSERT INTO t VALUES (7, 8, 9);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'second commit'), FALSE);",
                "UPDATE t SET c1 = 0 WHERE pk > 5;",
                "DELETE FROM t WHERE pk = 1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'third commit'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE to_pk = 1 ORDER BY to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE to_pk IS NULL ORDER BY to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("2"), T("3"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE to_pk IS NOT NULL ORDER BY to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                            &[T("7"), T("0"), T("9"), T("7"), T("8"), T("9"), T("modified")],
                            &[T("7"), T("8"), T("9"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE to_pk > 1 ORDER BY to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                            &[T("7"), T("0"), T("9"), T("7"), T("8"), T("9"), T("modified")],
                            &[T("7"), T("8"), T("9"), Null, Null, Null, T("added")],
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
            name: "selecting from_pk columns",
            intent: "Filters on dolt_diff_<t>'s from_ key columns return the rows whose old key matches.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'first commit'), FALSE);",
                "INSERT INTO t VALUES (7, 8, 9);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'second commit'), FALSE);",
                "UPDATE t SET c1 = 0 WHERE pk > 5;",
                "DELETE FROM t WHERE pk = 1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'third commit'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE from_pk = 1 ORDER BY to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("2"), T("3"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE from_pk IS NULL ORDER BY to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                            &[T("7"), T("8"), T("9"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE from_pk IS NOT NULL ORDER BY to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("7"), T("0"), T("9"), T("7"), T("8"), T("9"), T("modified")],
                            &[Null, Null, Null, T("1"), T("2"), T("3"), T("removed")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_DIFF_t WHERE from_pk > 1 ORDER BY to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("7"), T("0"), T("9"), T("7"), T("8"), T("9"), T("modified")],
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
            name: "selecting to_pk1 and to_pk2 columns",
            intent: "Filters on a compound key's to_ columns return the rows whose new key matches.",
            set_up_script: &[
                "CREATE TABLE t (pk1 int, pk2 int, c1 int, PRIMARY KEY (pk1, pk2));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'first commit'), FALSE);",
                "INSERT INTO t VALUES (7, 8, 9);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'second commit'), FALSE);",
                "UPDATE t SET c1 = 0 WHERE pk1 > 5;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'third commit'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_t;",
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
                    query: "SELECT to_pk1, to_pk2, to_c1, from_pk1, from_pk2, from_c1, dolt_diff_type FROM DOLT_DIFF_t WHERE to_pk1 = 1 ORDER BY to_pk1, to_pk2, to_c1, from_pk1, from_pk2, from_c1, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk1", INT4), Column("to_pk2", INT4), Column("to_c1", INT4), Column("from_pk1", INT4), Column("from_pk2", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk1, to_pk2, to_c1, from_pk1, from_pk2, from_c1, dolt_diff_type FROM DOLT_DIFF_t WHERE to_pk1 = 1 AND to_pk2 = 2 ORDER BY to_pk1, to_pk2, to_c1, from_pk1, from_pk2, from_c1, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk1", INT4), Column("to_pk2", INT4), Column("to_c1", INT4), Column("from_pk1", INT4), Column("from_pk2", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk1, to_pk2, to_c1, from_pk1, from_pk2, from_c1, dolt_diff_type FROM DOLT_DIFF_t WHERE to_pk1 > 1 AND to_pk2 < 10 ORDER BY to_pk1, to_pk2, to_c1, from_pk1, from_pk2, from_c1, dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk1", INT4), Column("to_pk2", INT4), Column("to_c1", INT4), Column("from_pk1", INT4), Column("from_pk2", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                            &[T("7"), T("8"), T("0"), T("7"), T("8"), T("9"), T("modified")],
                            &[T("7"), T("8"), T("9"), Null, Null, Null, T("added")],
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
            name: "Diff table shows diffs across primary key renames",
            intent: "Renaming key columns keeps the rows mapped by column, so dolt_diff_<t> shows them under the new names.",
            set_up_script: &[
                "CREATE TABLE t (pk1 int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "CREATE TABLE t2 (pk1a int, pk1b int, PRIMARY KEY (pk1a, pk1b));",
                "SELECT dolt_add('.');",
                "INSERT INTO t2 VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'initial');",
                "ALTER TABLE t RENAME COLUMN pk1 TO pk2;",
                "ALTER TABLE t2 RENAME COLUMN pk1a TO pk2a;",
                "ALTER TABLE t2 RENAME COLUMN pk1b TO pk2b;",
                "SELECT dolt_commit('-am', 'rename primary key');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk2, to_pk2, dolt_diff_type FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk2", INT4), Column("to_pk2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, T("1"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk2a, from_pk2b, to_pk2a, to_pk2b, dolt_diff_type FROM dolt_diff_t2;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk2a", INT4), Column("from_pk2b", INT4), Column("to_pk2a", INT4), Column("to_pk2b", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("2"), T("2"), T("added")],
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
            name: "add multiple columns, then set and unset a value. Should not show a diff",
            intent: "Setting a value and setting it back leaves no diff, even on a row stored before columns were added.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "UPDATE t SET col1 = 1 WHERE pk = 1;",
                "UPDATE t SET col1 = NULL WHERE pk = 1;",
                "SELECT dolt_commit('--allow-empty', '-am', 'fix short tuple');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_col1, from_pk, from_col1, dolt_diff_type FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_col1", INT4), Column("from_pk", INT4), Column("from_col1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, Null, Null, T("added")],
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
            name: "duplicate commit_hash",
            intent: "The dolt_diff table has a row for each table a commit changed, so one commit can appear twice.",
            set_up_script: &[
                "CREATE TABLE t1 (x int PRIMARY KEY);",
                "CREATE TABLE t2 (x int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.commit1', dolt_commit('-Am', 'commit1'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff WHERE commit_hash = current_setting('test.commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t1")],
                            &[T("public.t2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table drop and recreate with identical primary key keeps per-commit diff history",
            intent: "A table dropped and made again under its name with the same key keeps its diff history, the commit showing changed rows as modified.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY, name varchar(10));",
                "INSERT INTO t1 VALUES (1, 'dolt');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'create_table'), FALSE);",
                "UPDATE t1 SET name = 'doltgres' WHERE id = 1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'update_value'), FALSE);",
                "CREATE TABLE temp__t1 (id int PRIMARY KEY, name varchar(10));",
                "INSERT INTO temp__t1 VALUES (1, 'dolt');",
                "DROP TABLE t1;",
                "ALTER TABLE temp__t1 RENAME TO t1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-Am', 'drop and recreate'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t1;",
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
                    query: "SELECT to_id, to_name, from_id, from_name, dolt_diff_type FROM dolt_diff_t1 WHERE dolt_to_commit=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("to_name", VARCHAR), Column("from_id", INT4), Column("from_name", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("dolt"), T("1"), T("doltgres"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_id, to_name, from_id, from_name, dolt_diff_type FROM dolt_diff_t1 WHERE dolt_to_commit=current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("to_name", VARCHAR), Column("from_id", INT4), Column("from_name", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("doltgres"), T("1"), T("dolt"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_id, to_name, from_id, from_name, dolt_diff_type FROM dolt_diff_t1 WHERE dolt_to_commit=current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("to_name", VARCHAR), Column("from_id", INT4), Column("from_name", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("dolt"), Null, Null, T("added")],
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
            name: "table drop and recreate with identical decimal primary key keeps per-commit diff history",
            intent: "A table dropped and made again with the same numeric key keeps its diff history.",
            set_up_script: &[
                "CREATE TABLE t1 (id decimal(10,2) PRIMARY KEY, name varchar(10));",
                "INSERT INTO t1 VALUES (1, 'dolt');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'create_table'), FALSE);",
                "UPDATE t1 SET name = 'doltgres' WHERE id = 1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'update_value'), FALSE);",
                "CREATE TABLE temp__t1 (id decimal(10,2) PRIMARY KEY, name varchar(10));",
                "INSERT INTO temp__t1 VALUES (1, 'dolt');",
                "DROP TABLE t1;",
                "ALTER TABLE temp__t1 RENAME TO t1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-Am', 'drop and recreate'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t1;",
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
                    query: "SELECT to_name, from_name, dolt_diff_type FROM dolt_diff_t1 WHERE dolt_to_commit=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("to_name", VARCHAR), Column("from_name", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("dolt"), T("doltgres"), T("modified")],
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
            name: "table drop and recreate with identical enum primary key keeps per-commit diff history",
            intent: "A table dropped and made again with the same enum key keeps its diff history.",
            set_up_script: &[
                "CREATE TYPE id_enum AS ENUM ('a', 'b');",
                "CREATE TABLE t1 (id id_enum PRIMARY KEY, name varchar(10));",
                "INSERT INTO t1 VALUES ('a', 'dolt');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'create_table'), FALSE);",
                "UPDATE t1 SET name = 'doltgres' WHERE id = 'a';",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'update_value'), FALSE);",
                "CREATE TABLE temp__t1 (id id_enum PRIMARY KEY, name varchar(10));",
                "INSERT INTO temp__t1 VALUES ('a', 'dolt');",
                "DROP TABLE t1;",
                "ALTER TABLE temp__t1 RENAME TO t1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-Am', 'drop and recreate'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's inline enum is a Postgres enum type here, and its first label 'a' stands for the 1 Dolt inserts")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_name, from_name, dolt_diff_type FROM dolt_diff_t1 WHERE dolt_to_commit=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("to_name", VARCHAR), Column("from_name", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("dolt"), T("doltgres"), T("modified")],
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
            name: "crossed primary key tags warn in diff instead of diffing",
            intent: "Key columns whose tags were crossed make dolt_diff_<t> warn instead of mapping the wrong columns.",
            set_up_script: &[
                "CREATE TABLE t1 (a int, b int, PRIMARY KEY (a, b));",
                "SELECT dolt_update_column_tag('t1', 'a', '9001');",
                "SELECT dolt_update_column_tag('t1', 'b', '9002');",
                "INSERT INTO t1 VALUES (1, 2);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'create_table'), FALSE);",
                "SELECT dolt_update_column_tag('t1', 'b', '9003');",
                "SELECT dolt_update_column_tag('t1', 'a', '9002');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'cross tags'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t1;",
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
            skip: Some("Dolt-only: dolt_update_column_tag sets Dolt's column tags, and Doltgrust's column ids are its own and can't be set, Appendix C \"column tags\""),
            ..S
        },
    ]);
}

#[test]
fn test_diff_table_function_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "IN clause with NOT IN correctly returns table names from DOLT_DIFF",
            intent: "IN and NOT IN subqueries filter the dolt_diff table's table names.",
            set_up_script: &[
                "CREATE TABLE test_table (id int PRIMARY KEY, name VARCHAR(50));",
                "INSERT INTO test_table VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test_table VALUES (4, 'David'), (5, 'Eve');",
                "SELECT dolt_commit('-am', 'Add David and Eve');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test_table VALUES (6, 'Frank');",
                "UPDATE test_table SET name = 'Alice Updated' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'Update Alice and add Frank');",
                "CREATE TABLE filter_table (name VARCHAR(50));",
                "INSERT INTO filter_table VALUES ('Alice Updated'), ('Bob'), ('Charlie'), ('David');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff WHERE commit_hash = dolt_hashof('main') \n						  AND data_change = TRUE \n						  AND table_name NOT IN (SELECT 'dropped_table')\n						  AND table_name IN (SELECT 'public.filter_table' UNION SELECT 'public.test_table');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.test_table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff WHERE commit_hash = dolt_hashof('main') \n						  AND data_change = TRUE \n						  AND table_name NOT IN (SELECT 'dropped_table')\n						  AND table_name IN (SELECT 'public.other_name' UNION SELECT 'public.another_name');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff WHERE commit_hash = dolt_hashof('main') \n						  AND data_change = TRUE \n						  AND table_name IN (SELECT 'public.other_name' UNION SELECT 'public.another_name');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
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
            name: "dolt_diff: SELECT * skinny schema visibility",
            intent: "dolt_diff's --skinny leaves out columns no row changed, --include-cols keeps the ones it names, and a view over either has those columns.",
            set_up_script: &[
                "CREATE TABLE t (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk)); COMMENT ON COLUMN t.pk IS 'tag:0'; COMMENT ON COLUMN t.c1 IS 'tag:1'; COMMENT ON COLUMN t.c2 IS 'tag:2'; COMMENT ON COLUMN t.c3 IS 'tag:3'; COMMENT ON COLUMN t.c4 IS 'tag:4'; COMMENT ON COLUMN t.c5 IS 'tag:5';",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.C0', ('')::text, FALSE);",
                "SELECT set_config('test.C0', dolt_commit('-m', 'Created table t'), FALSE);",
                "INSERT INTO t VALUES (0,1,2,3,4,5), (1,1,2,3,4,5);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.C1', ('')::text, FALSE);",
                "SELECT set_config('test.C1', dolt_commit('-m', 'Added initial data'), FALSE);",
                "UPDATE t SET c1=100, c3=300 WHERE pk=0;",
                "UPDATE t SET c2=200 WHERE pk=1;",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.C2', ('')::text, FALSE);",
                "SELECT set_config('test.C2', dolt_commit('-m', 'Updated some columns'), FALSE);",
                "ALTER TABLE t ADD COLUMN c6 bigint;",
                "UPDATE t SET c6=600 WHERE pk=0;",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.C3', ('')::text, FALSE);",
                "SELECT set_config('test.C3', dolt_commit('-m', 'Added new column and updated it'), FALSE);",
                "DELETE FROM t WHERE pk=1;",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.C4', ('')::text, FALSE);",
                "SELECT set_config('test.C4', dolt_commit('-m', 'Deleted a row'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT d.to_pk, d.to_c1, d.to_c2, d.to_c3, d.to_c4, d.to_c5, d.from_pk, d.from_c1, d.from_c2, d.from_c3, d.from_c4, d.from_c5, d.dolt_diff_type FROM (SELECT * FROM dolt_diff('--skinny', current_setting('test.C0'), current_setting('test.C1'), 't')) d ORDER BY COALESCE(d.to_pk, d.from_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT8), Column("to_c1", INT8), Column("to_c2", INT8), Column("to_c3", INT8), Column("to_c4", INT8), Column("to_c5", INT8), Column("from_pk", INT8), Column("from_c1", INT8), Column("from_c2", INT8), Column("from_c3", INT8), Column("from_c4", INT8), Column("from_c5", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5"), Null, Null, Null, Null, Null, Null, T("added")],
                            &[T("1"), T("1"), T("2"), T("3"), T("4"), T("5"), Null, Null, Null, Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.to_pk, d.to_c1, d.to_c2, d.to_c3, d.from_pk, d.from_c1, d.from_c2, d.from_c3, d.dolt_diff_type FROM (SELECT * FROM dolt_diff(current_setting('test.C1'), current_setting('test.C2'), 't')) d ORDER BY COALESCE(d.to_pk, d.from_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT8), Column("to_c1", INT8), Column("to_c2", INT8), Column("to_c3", INT8), Column("from_pk", INT8), Column("from_c1", INT8), Column("from_c2", INT8), Column("from_c3", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("100"), T("2"), T("300"), T("0"), T("1"), T("2"), T("3"), T("modified")],
                            &[T("1"), T("1"), T("200"), T("3"), T("1"), T("1"), T("2"), T("3"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.to_pk, d.to_c1, d.to_c2, d.to_c3, d.dolt_diff_type FROM (SELECT * FROM dolt_diff('--skinny', current_setting('test.C1'), current_setting('test.C2'), 't')) d ORDER BY d.to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT8), Column("to_c1", INT8), Column("to_c2", INT8), Column("to_c3", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("100"), T("2"), T("300"), T("modified")],
                            &[T("1"), T("1"), T("200"), T("3"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.to_pk, d.to_c6, d.dolt_diff_type FROM (SELECT * FROM dolt_diff('--skinny', current_setting('test.C2'), current_setting('test.C3'), 't')) d;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT8), Column("to_c6", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("600"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.to_pk, d.to_c1, d.to_c2, d.to_c6, d.dolt_diff_type FROM (SELECT * FROM dolt_diff('--skinny', '--include-cols=c1,c2', current_setting('test.C2'), current_setting('test.C3'), 't')) d;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT8), Column("to_c1", INT8), Column("to_c2", INT8), Column("to_c6", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("100"), T("2"), T("600"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.from_pk, d.from_c1, d.from_c2, d.from_c3, d.from_c4, d.from_c5, d.from_c6, d.dolt_diff_type FROM (SELECT * FROM dolt_diff('--skinny', current_setting('test.C3'), current_setting('test.C4'), 't')) d;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("from_c1", INT8), Column("from_c2", INT8), Column("from_c3", INT8), Column("from_c4", INT8), Column("from_c5", INT8), Column("from_c6", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("200"), T("3"), T("4"), T("5"), Null, T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_all_01;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_all_01" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_all_01 AS SELECT * FROM dolt_diff(current_setting('test.C0'), current_setting('test.C1'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_all_01' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_all_01;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_skinny_01;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_skinny_01" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_skinny_01 AS SELECT * FROM dolt_diff('--skinny', current_setting('test.C0'), current_setting('test.C1'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_skinny_01' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_skinny_01;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_all_12;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_all_12" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_all_12 AS SELECT * FROM dolt_diff(current_setting('test.C1'), current_setting('test.C2'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_all_12' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_all_12;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_skinny_12;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_skinny_12" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_skinny_12 AS SELECT * FROM dolt_diff('--skinny', current_setting('test.C1'), current_setting('test.C2'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_skinny_12' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_skinny_12;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_all_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_all_23" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_all_23 AS SELECT * FROM dolt_diff(current_setting('test.C2'), current_setting('test.C3'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_all_23' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_all_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_skinny_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_skinny_23" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_skinny_23 AS SELECT * FROM dolt_diff('--skinny', current_setting('test.C2'), current_setting('test.C3'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_skinny_23' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
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
                ScriptTestAssertion {
                    query: "DROP VIEW v_skinny_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_skinny_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_skinny_23" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_skinny_23 AS SELECT * FROM dolt_diff(current_setting('test.C2'), current_setting('test.C3'), 't', '--skinny');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_skinny_23' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
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
                ScriptTestAssertion {
                    query: "DROP VIEW v_skinny_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_skinny_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_skinny_23" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_skinny_23 AS SELECT * FROM dolt_diff('--skinny', '--include-cols=c1,c2', current_setting('test.C2'), current_setting('test.C3'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_skinny_23' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_skinny_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_skinny_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_skinny_23" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_skinny_23 AS SELECT * FROM dolt_diff('--skinny', '--include-cols=c1,c2,c6', current_setting('test.C2'), current_setting('test.C3'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_skinny_23' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_skinny_23;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_all_34;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_all_34" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_all_34 AS SELECT * FROM dolt_diff(current_setting('test.C3'), current_setting('test.C4'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_all_34' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_all_34;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW IF EXISTS v_skinny_34;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "v_skinny_34" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v_skinny_34 AS SELECT * FROM dolt_diff('--skinny', current_setting('test.C3'), current_setting('test.C4'), 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT REPLACE(REPLACE(column_name, 'to_', ''), 'from_', '')) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'v_skinny_34' AND column_name NOT IN ('dolt_to_commit', 'dolt_from_commit', 'dolt_to_commit_date', 'dolt_from_commit_date', 'dolt_diff_type');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v_skinny_34;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "invalid arguments",
            intent: "dolt_diff refuses wrong argument counts, NULLs, missing tables and revisions, and arguments it can't evaluate before the query runs.",
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
                    query: "SELECT * FROM dolt_diff();",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("committer", TEXT), Column("email", TEXT), Column("date", TIMESTAMPTZ), Column("message", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL), Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMPTZ)],
                        rows: &[
                            &[Any, T("public.t"), Any, Any, Any, T("inserting into t"), T("t"), T("f"), Any, Any, Any],
                            &[Any, T("public.t"), Any, Any, Any, T("creating table t"), T("f"), T("t"), Any, Any, Any],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust's dolt_diff without arguments is the dolt_diff table, as the view of that name reads it, Appendix C \"dolt_diff without arguments\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('t');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff' expected 2 to 3 arguments, 1 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(current_setting('test.Commit1'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff' expected 3 arguments, 2 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 'extra', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff' expected 3 arguments, 4 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(NULL, NULL, NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_diff: NULL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff(text, text, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(123, current_setting('test.Commit2'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff(integer, text, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(current_setting('test.Commit1'), 123, 't');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff(text, integer, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 'doesnotexist');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('fakefakefakefakefakefakefakefake', current_setting('test.Commit2'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(current_setting('test.Commit1'), 'fake-branch', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(current_setting('test.Commit1'), concat('fake', '-', 'branch'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(dolt_hashof('main'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff(dolt_hashof('main'), current_setting('test.Commit2'), LOWER('T'));",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main..main~');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff' expected 2 to 3 arguments, 1 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main..main~', 'extra', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff with .. or ...' expected 2 arguments, 3 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main..main^', 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff(unknown, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main..main~', 'doesnotexist');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('fakefakefakefakefakefakefakefake..main', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main..fakefakefakefakefakefakefakefake', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('fakefakefakefakefakefakefakefake...main', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main...fakefakefakefakefakefakefakefake', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main..main~', LOWER('T'));",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("main~"), Any, T("1"), T("one"), T("two"), T("main"), Any, T("removed")],
                            &[Null, Null, Null, T("main~"), Any, T("2"), T("two"), T("three"), T("main"), Any, T("removed")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff: column reference commit arguments are rejected with a clear error",
            intent: "dolt_diff refuses an argument that reads a column of an outer query, and runs with constant arguments.",
            set_up_script: &[
                "CREATE TABLE airspaces (id int PRIMARY KEY, name VARCHAR(20));",
                "INSERT INTO airspaces VALUES (1, 'a');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'init');",
                "SELECT dolt_branch('branch-x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT branch.name, (SELECT COUNT(*) FROM DOLT_DIFF(branch.name, 'branch-x', 'airspaces')) FROM dolt_branches branch;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_diff: branch.name – only literal values supported", ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch.name, (SELECT COUNT(*) FROM DOLT_DIFF(CONCAT(branch.name, '...branch-x'), 'airspaces')) FROM dolt_branches branch;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_diff: concat(branch.name, '...branch-x') – only literal values supported", ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch.name, (SELECT COUNT(*) FROM DOLT_DIFF('main', 'branch-x', branch.name)) FROM dolt_branches branch;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_diff: branch.name – only literal values supported", ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF('main', 'branch-x', 'airspaces');",
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
            name: "dolt_diff: per branch diffs run as one literal statement per branch",
            intent: "Dynamic SQL that calls dolt_diff once per branch with that branch's revisions counts the rows both sides changed.",
            set_up_script: &[
                "CREATE TABLE airspaces (id int PRIMARY KEY, name VARCHAR(20));",
                "INSERT INTO airspaces VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'init');",
                "SELECT dolt_branch('branch-x');",
                "SELECT dolt_branch('branch-y');",
                "SELECT * FROM dolt_checkout('branch-x');",
                "UPDATE airspaces SET name = 'x1' WHERE id = 1;",
                "UPDATE airspaces SET name = 'x2' WHERE id = 2;",
                "SELECT dolt_commit('-am', 'x edits');",
                "SELECT * FROM dolt_checkout('branch-y');",
                "UPDATE airspaces SET name = 'y2' WHERE id = 2;",
                "UPDATE airspaces SET name = 'y3' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'y edits');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('-b', 'widened');",
                "ALTER TABLE airspaces ADD COLUMN altitude int;",
                "UPDATE airspaces SET name = 'w2', altitude = 100 WHERE id = 2;",
                "SELECT dolt_commit('-am', 'widen and edit');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE FUNCTION airspace_warnings(base varchar(64)) RETURNS TABLE (branch varchar, warning_count integer) LANGUAGE plpgsql AS $$ DECLARE b varchar(64); BEGIN DROP TABLE IF EXISTS warning_counts; CREATE TEMPORARY TABLE warning_counts (branch varchar(64), warning_count int); FOR b IN SELECT name FROM dolt_branches WHERE name <> base LOOP EXECUTE format('INSERT INTO warning_counts SELECT $1, COUNT(*) FROM (SELECT COALESCE(to_id, from_id) AS id FROM dolt_diff(%L, ''airspaces'') INTERSECT SELECT COALESCE(to_id, from_id) FROM dolt_diff(%L, ''airspaces'')) co', b || '...' || base, base || '...' || b) USING b; END LOOP; RETURN QUERY SELECT w.branch, w.warning_count FROM warning_counts w ORDER BY w.branch; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM airspace_warnings('branch-x');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", VARCHAR), Column("warning_count", INT4)],
                        rows: &[
                            &[T("branch-y"), T("1")],
                            &[T("main"), T("0")],
                            &[T("widened"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    notices: &[Diagnostic { code: "00000", message: r#"table "warning_counts" does not exist, skipping"#, ..N }],
                    dolt: Some(Dolt::MySqlOnly("MySQL's procedure with a cursor and PREPARE is a PL/pgSQL function here, which runs each statement with EXECUTE and returns the rows of the temporary table it fills")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM warning_counts ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", VARCHAR), Column("warning_count", INT4)],
                        rows: &[
                            &[T("branch-y"), T("1")],
                            &[T("main"), T("0")],
                            &[T("widened"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case",
            intent: "dolt_diff between two commits shows each changed row of the table, in either direction, with filters and joins.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "CREATE TABLE t2 (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t2 VALUES(100, 'hundred', 'hundert');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into table t2'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four');",
                "UPDATE t SET c1='uno', c2='dos' WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting into table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
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
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("uno"), T("dos"), T("1"), T("one"), T("two"), T("modified")],
                            &[T("2"), T("two"), T("three"), Null, Null, Null, T("added")],
                            &[T("3"), T("three"), T("four"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit4'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), T("1"), T("uno"), T("dos"), T("modified")],
                            &[Null, Null, Null, T("2"), T("two"), T("three"), T("removed")],
                            &[Null, Null, Null, T("3"), T("three"), T("four"), T("removed")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type  FROM dolt_diff(current_setting('test.Commit3'), current_setting('test.Commit4'), 'T2');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type  FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("uno"), T("dos"), Null, Null, Null, T("added")],
                            &[T("2"), T("two"), T("three"), Null, Null, Null, T("added")],
                            &[T("3"), T("three"), T("four"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type  FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit4'), 't') WHERE to_pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("two"), T("three"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type  FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit4'), 't') WHERE from_pk IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("uno"), T("dos"), Null, Null, Null, T("added")],
                            &[T("2"), T("two"), T("three"), Null, Null, Null, T("added")],
                            &[T("3"), T("three"), T("four"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type  FROM dolt_diff(current_setting('test.Commit4'), current_setting('test.Commit1'), 'T');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("uno"), T("dos"), T("removed")],
                            &[Null, Null, Null, T("2"), T("two"), T("three"), T("removed")],
                            &[Null, Null, Null, T("3"), T("three"), T("four"), T("removed")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type  FROM dolt_diff(current_setting('test.Commit4'), current_setting('test.Commit1'), 'T') WHERE from_pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("2"), T("two"), T("three"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type  FROM dolt_diff(current_setting('test.Commit4'), current_setting('test.Commit1'), 'T') WHERE to_pk IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("uno"), T("dos"), T("removed")],
                            &[Null, Null, Null, T("2"), T("two"), T("three"), T("removed")],
                            &[Null, Null, Null, T("3"), T("three"), T("four"), T("removed")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type\nFROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 't')\nINNER JOIN t ON to_pk = t.pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, from_c1, to_c1, from_c1, to_c1, dolt_diff_type, dolt_diff_type\nFROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 't') CROSS JOIN dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42712", message: r#"table name "dolt_diff" specified more than once"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's INNER JOIN without ON is a CROSS JOIN in Postgres, where two FROM items of one name fail before any column is resolved")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.to_pk, a.from_c1, a.to_c1, b.from_c1, b.to_c1, a.dolt_diff_type, b.dolt_diff_type\nFROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 't') a INNER JOIN dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit3'), 't') b\nON a.to_pk = b.to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("from_c1", VARCHAR), Column("to_c1", VARCHAR), Column("from_c1", VARCHAR), Column("to_c1", VARCHAR), Column("dolt_diff_type", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("one"), Null, T("one"), T("added"), T("added")],
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
            name: "WORKING and STAGED",
            intent: "dolt_diff reads WORKING and STAGED as revisions, alone and in two-dot ranges, and staging moves the rows between them.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text, c2 text);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'three', 'four');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'inserting two rows into table t'), FALSE);",
                "INSERT INTO t VALUES (3, 'five', 'six');",
                "DELETE FROM t WHERE pk = 2;",
                "UPDATE t SET c2 = '100' WHERE pk = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1'), 'WORKING', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), T("1"), T("one"), T("100"), T("modified")],
                            &[T("2"), T("three"), T("four"), Null, Null, Null, T("removed")],
                            &[Null, Null, Null, T("3"), T("five"), T("six"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), T("1"), T("one"), T("100"), T("modified")],
                            &[T("2"), T("three"), T("four"), Null, Null, Null, T("removed")],
                            &[Null, Null, Null, T("3"), T("five"), T("six"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff('STAGED..WORKING', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), T("1"), T("one"), T("100"), T("modified")],
                            &[T("2"), T("three"), T("four"), Null, Null, Null, T("removed")],
                            &[Null, Null, Null, T("3"), T("five"), T("six"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff('WORKING', 'STAGED', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("100"), T("1"), T("one"), T("two"), T("modified")],
                            &[Null, Null, Null, T("2"), T("three"), T("four"), T("added")],
                            &[T("3"), T("five"), T("six"), Null, Null, Null, T("removed")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff('WORKING', 'WORKING', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff('WORKING..WORKING', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff('STAGED', 'STAGED', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff('WORKING', 'STAGED', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff('HEAD', 'STAGED', 't') ORDER by coalesce(from_pk, to_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", TEXT), Column("from_c2", TEXT), Column("to_pk", INT4), Column("to_c1", TEXT), Column("to_c2", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), T("1"), T("one"), T("100"), T("modified")],
                            &[T("2"), T("three"), T("four"), Null, Null, Null, T("removed")],
                            &[Null, Null, Null, T("3"), T("five"), T("six"), T("added")],
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
            name: "diff with branch refs",
            intent: "dolt_diff takes branches, ancestry specs, two-dot ranges, and three-dot ranges from the merge base.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting row 1 into t in main'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'dropping column c2 in branch1'), FALSE);",
                "DELETE FROM t WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'deleting row 1 in branch1'), FALSE);",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'inserting row 2 in branch1'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'two', 'three');",
                "SELECT set_config('test.Commit6', ('')::text, FALSE);",
                "SELECT set_config('test.Commit6', dolt_commit('-am', 'inserting row 2 in main'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("1"), T("one"), T("two"), T("removed")],
                            &[T("2"), T("two"), T("2"), T("two"), T("three"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff('main..branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("1"), T("one"), T("two"), T("removed")],
                            &[T("2"), T("two"), T("2"), T("two"), T("three"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, dolt_diff_type FROM dolt_diff('branch1', 'main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), Null, Null, T("added")],
                            &[T("2"), T("two"), T("three"), T("2"), T("two"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, dolt_diff_type FROM dolt_diff('branch1..main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), Null, Null, T("added")],
                            &[T("2"), T("two"), T("three"), T("2"), T("two"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff('main~', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("1"), T("one"), T("two"), T("removed")],
                            &[T("2"), T("two"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff('main~..branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("1"), T("one"), T("two"), T("removed")],
                            &[T("2"), T("two"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff('main...branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("1"), T("one"), T("two"), T("removed")],
                            &[T("2"), T("two"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, dolt_diff_type FROM dolt_diff('branch1...main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("two"), T("three"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff('main~...branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("1"), T("one"), T("two"), T("removed")],
                            &[T("2"), T("two"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff('main...branch1~', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("1"), T("one"), T("two"), T("removed")],
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
            name: "schema modification: drop and recreate column with same type",
            intent: "A column dropped and added again shows in each side's columns only where that side has it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'dropping column c2'), FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(20);",
                "INSERT INTO t VALUES (3, 'three', 'four');",
                "UPDATE t SET c2='foo' WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'adding column c2, inserting, and updating data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), Null, Null, Null, T("added")],
                            &[T("2"), T("two"), T("three"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("1"), T("one"), T("two"), T("modified")],
                            &[T("2"), T("two"), T("2"), T("two"), T("three"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_c2 FROM dolt_diff(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "to_c2" does not exist"#, hint: "Perhaps you meant to reference the column \"dolt_diff.to_c1\".", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, dolt_diff_type FROM dolt_diff(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("foo"), T("1"), T("one"), T("modified")],
                            &[T("3"), T("three"), T("four"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c2 FROM dolt_diff(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "from_c2" does not exist"#, hint: "Perhaps you meant to reference the column \"dolt_diff.from_c1\".", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("foo"), Null, Null, Null, T("added")],
                            &[T("2"), T("two"), Null, Null, Null, Null, T("added")],
                            &[T("3"), T("three"), T("four"), Null, Null, Null, T("added")],
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
            name: "schema modification: rename columns",
            intent: "A renamed column shows under each side's own name, so the other name is missing on that side.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 int);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', -1), (2, 'two', -2);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c2 TO c3;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'renaming column c2 to c3'), FALSE);",
                "INSERT INTO t VALUES (3, 'three', -3);",
                "UPDATE t SET c3=1 WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting and updating data'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c3 TO c2;",
                "INSERT INTO t VALUES (4, 'four', -4);",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'renaming column c3 to c2, and inserting data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("-1"), Null, Null, Null, T("added")],
                            &[T("2"), T("two"), T("-2"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_c2 FROM dolt_diff(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "to_c2" does not exist"#, hint: "Perhaps you meant to reference the column \"dolt_diff.to_c1\" or the column \"dolt_diff.to_c3\".", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c3, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c3", INT4), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c3, from_pk, from_c1, from_c3, dolt_diff_type FROM dolt_diff(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c3", INT4), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c3", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), T("three"), T("-3"), Null, Null, Null, T("added")],
                            &[T("1"), T("one"), T("1"), T("1"), T("one"), T("-1"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c2 FROM dolt_diff(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "from_c2" does not exist"#, hint: "Perhaps you meant to reference the column \"dolt_diff.from_c1\" or the column \"dolt_diff.from_c3\".", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_c3 FROM dolt_diff(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "to_c3" does not exist"#, hint: "Perhaps you meant to reference the column \"dolt_diff.to_c1\" or the column \"dolt_diff.to_c2\".", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c3, dolt_diff_type FROM dolt_diff(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c3", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("four"), T("-4"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("1"), Null, Null, Null, T("added")],
                            &[T("2"), T("two"), T("-2"), Null, Null, Null, T("added")],
                            &[T("3"), T("three"), T("-3"), Null, Null, Null, T("added")],
                            &[T("4"), T("four"), T("-4"), Null, Null, Null, T("added")],
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
            name: "schema modification: drop and rename columns with different types",
            intent: "A column added again with another type shows each side's own type, so a diff across both has both types.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'asdf'), (2, 'two', '2');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'dropping column c2'), FALSE);",
                "INSERT INTO t VALUES (3, 'three');",
                "UPDATE t SET c1='fdsa' WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting and updating data'), FALSE);",
                "ALTER TABLE t ADD COLUMN c2 int;",
                "INSERT INTO t VALUES (4, 'four', -4);",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'adding column c2, and inserting data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("asdf"), Null, Null, Null, T("added")],
                            &[T("2"), T("two"), T("2"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("1"), T("one"), T("asdf"), T("modified")],
                            &[T("2"), T("two"), T("2"), T("two"), T("2"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), T("three"), Null, Null, T("added")],
                            &[T("1"), T("fdsa"), T("1"), T("one"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, dolt_diff_type FROM dolt_diff(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("four"), T("-4"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("fdsa"), Null, Null, Null, Null, T("added")],
                            &[T("2"), T("two"), Null, Null, Null, Null, T("added")],
                            &[T("3"), T("three"), Null, Null, Null, Null, T("added")],
                            &[T("4"), T("four"), T("-4"), Null, Null, Null, T("added")],
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
            name: "new table",
            intent: "A table only one side has shows all its rows as added or removed, with NULL columns on the other side.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1,2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_a, to_b, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("to_a", INT4), Column("to_b", INT4), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("HEAD"), T("WORKING"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_a, from_b, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("to_a", INT4), Column("from_b", INT4), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("HEAD"), T("WORKING"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_a, from_b, to_a, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff('WORKING', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_a", INT4), Column("from_b", INT4), Column("to_a", INT4), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), Null, T("WORKING"), T("HEAD"), T("removed")],
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
            name: "dropped table",
            intent: "A table dropped in the to commit shows all its rows as removed.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2);",
                "SELECT dolt_commit('-am', 'new table');",
                "DROP TABLE t1;",
                "SELECT dolt_commit('-am', 'dropped table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_a, from_b, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff('HEAD~', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_a", INT4), Column("from_b", INT4), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("HEAD~"), T("HEAD"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_a, from_b, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff('HEAD~..HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_a", INT4), Column("from_b", INT4), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("HEAD~"), T("HEAD"), T("removed")],
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
            name: "renamed table",
            intent: "A renamed table is followed across the rename by either name, showing only the rows that changed.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2);",
                "SELECT dolt_commit('-am', 'new table');",
                "ALTER TABLE t1 RENAME TO t2;",
                "SELECT dolt_add('.');",
                "INSERT INTO t2 VALUES (3,4);",
                "SELECT dolt_commit('-am', 'renamed table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_a, to_b, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff('HEAD~', 'HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("to_a", INT4), Column("to_b", INT4), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), T("4"), T("HEAD~"), T("HEAD"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_a, to_b, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff('HEAD~..HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("to_a", INT4), Column("to_b", INT4), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), T("4"), T("HEAD~"), T("HEAD"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_a, to_b, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff('HEAD~', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("to_a", INT4), Column("to_b", INT4), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), T("4"), T("HEAD~"), T("HEAD"), T("added")],
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
            name: "Renaming a primary key column shows PK values in both the to and from columns",
            intent: "Key columns renamed between the commits still match rows, so the key shows on both sides.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t1 VALUES (1, 1);",
                "CREATE TABLE t2 (pk1a int, pk1b int, col1 int, PRIMARY KEY (pk1a, pk1b));",
                "INSERT INTO t2 VALUES (1, 1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial');",
                "ALTER TABLE t1 RENAME COLUMN pk TO pk2;",
                "UPDATE t1 SET col1 = 100;",
                "ALTER TABLE t2 RENAME COLUMN pk1a TO pk2a;",
                "ALTER TABLE t2 RENAME COLUMN pk1b TO pk2b;",
                "UPDATE t2 SET col1 = 100;",
                "SELECT dolt_commit('-am', 'edit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk2, to_col1, from_pk, from_col1, dolt_diff_type FROM dolt_diff('HEAD~', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk2", INT4), Column("to_col1", INT4), Column("from_pk", INT4), Column("from_col1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("100"), T("1"), T("1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk2, to_col1, from_pk, from_col1, dolt_diff_type FROM dolt_diff('HEAD~..HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk2", INT4), Column("to_col1", INT4), Column("from_pk", INT4), Column("from_col1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("100"), T("1"), T("1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk2a, to_pk2b, to_col1, from_pk1a, from_pk1b, from_col1, dolt_diff_type FROM dolt_diff('HEAD~', 'HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk2a", INT4), Column("to_pk2b", INT4), Column("to_col1", INT4), Column("from_pk1a", INT4), Column("from_pk1b", INT4), Column("from_col1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("100"), T("1"), T("1"), T("1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk2a, to_pk2b, to_col1, from_pk1a, from_pk1b, from_col1, dolt_diff_type FROM dolt_diff('HEAD~..HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk2a", INT4), Column("to_pk2b", INT4), Column("to_col1", INT4), Column("from_pk1a", INT4), Column("from_pk1b", INT4), Column("from_col1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("100"), T("1"), T("1"), T("1"), T("modified")],
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
            name: "diff on dolt_schemas on events",
            intent: "Changing a MySQL event shows in a diff of dolt_schemas.",
            set_up_script: &[
                "CREATE TABLE messages (id int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, message VARCHAR(255) NOT NULL, created_at timestamp NOT NULL);",
                "CREATE EVENT IF NOT EXISTS msg_event ON SCHEDULE AT CURRENT_TIMESTAMP + INTERVAL 1 YEAR DISABLE DO INSERT INTO messages(message,created_at) VALUES('Test Dolt Event 1',NOW());",
                "CREATE EVENT my_commit ON SCHEDULE EVERY 1 DAY DISABLE DO CALL DOLT_COMMIT('--allow-empty','-am','my daily commit');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'Creating table and events'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT type, name FROM dolt_schemas;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("event"), T("msg_event")],
                            &[T("event"), T("my_commit")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT msg_event ON SCHEDULE AT CURRENT_TIMESTAMP + INTERVAL 1 YEAR DISABLE DO INSERT INTO messages(message,created_at) VALUES('Test Dolt Event 2',NOW());",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "sql.ErrEventAlreadyExists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT msg_event;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT msg_event ON SCHEDULE AT CURRENT_TIMESTAMP + INTERVAL 1 YEAR ON COMPLETION PRESERVE DISABLE DO INSERT INTO messages(message,created_at) VALUES('Test Dolt Event 2',NOW());",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_type, from_name, to_name, dolt_diff_type FROM DOLT_DIFF('HEAD', 'WORKING', 'dolt_schemas');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT), Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("event"), T("msg_event"), T("msg_event"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT type, name FROM dolt_schemas;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("event"), T("msg_event")],
                            &[T("event"), T("my_commit")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: events are MySQL's, and Postgres has none"),
            ..S
        },
        ScriptTest {
            name: "diff table function works with views",
            intent: "A view over dolt_diff resolves its revisions each time it runs, so it follows HEAD.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'created table');",
                "INSERT INTO t VALUES (1), (2), (3);",
                "SELECT dolt_commit('-Am', 'inserted into table');",
                "CREATE VIEW v AS SELECT to_i, dolt_to_commit AS to_commit, from_i, dolt_from_commit AS from_commit, dolt_diff_type AS diff_type FROM dolt_diff('HEAD', 'HEAD~1', 't');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM v;",
                    expected: Expected::Rows {
                        columns: &[Column("to_i", INT4), Column("to_commit", TEXT), Column("from_i", INT4), Column("from_commit", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[Null, T("HEAD~1"), T("1"), T("HEAD"), T("removed")],
                            &[Null, T("HEAD~1"), T("2"), T("HEAD"), T("removed")],
                            &[Null, T("HEAD~1"), T("3"), T("HEAD"), T("removed")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4), (5), (6);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'inserted into table again');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v;",
                    expected: Expected::Rows {
                        columns: &[Column("to_i", INT4), Column("to_commit", TEXT), Column("from_i", INT4), Column("from_commit", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[Null, T("HEAD~1"), T("4"), T("HEAD"), T("removed")],
                            &[Null, T("HEAD~1"), T("5"), T("HEAD"), T("removed")],
                            &[Null, T("HEAD~1"), T("6"), T("HEAD"), T("removed")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff table function works with virtual generated columns",
            intent: "dolt_diff reads NULL for a virtual generated column, which no stored row holds.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int, k int, jk int generated always AS (10 * j + k), l int);",
                "SELECT dolt_commit('-Am', 'created table');",
                "INSERT INTO t(i, j, k, l) VALUES (1, 2, 3, 4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("k", INT4), Column("jk", INT4), Column("l", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("23"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_i, to_jk, from_i, from_jk FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("to_i", INT4), Column("to_jk", INT4), Column("from_i", INT4), Column("from_jk", INT4)],
                        rows: &[
                            &[T("1"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_i, to_jk, from_i, from_jk FROM dolt_diff('HEAD', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_i", INT4), Column("to_jk", INT4), Column("from_i", INT4), Column("from_jk", INT4)],
                        rows: &[
                            &[T("1"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff table function works with stored generated columns",
            intent: "dolt_diff reads a stored generated column's values.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int, k int, jk int generated always AS (10 * j + k) stored, l int);",
                "SELECT dolt_commit('-Am', 'created table');",
                "INSERT INTO t(i, j, k, l) VALUES (1, 2, 3, 4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("k", INT4), Column("jk", INT4), Column("l", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("23"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_i, to_jk, from_i, from_jk FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("to_i", INT4), Column("to_jk", INT4), Column("from_i", INT4), Column("from_jk", INT4)],
                        rows: &[
                            &[T("1"), T("23"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_i, to_jk, from_i, from_jk FROM dolt_diff('HEAD', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_i", INT4), Column("to_jk", INT4), Column("from_i", INT4), Column("from_jk", INT4)],
                        rows: &[
                            &[T("1"), T("23"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff commit-to-commit shows tables added to dolt_ignore after the commit",
            intent: "Ignoring a table later doesn't hide its rows from a diff between earlier commits.",
            set_up_script: &[
                "CREATE TABLE the_table (pk int PRIMARY KEY, c1 int);",
                "INSERT INTO the_table VALUES (1, 10), (2, 20);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'add the_table');",
                "INSERT INTO dolt_ignore VALUES ('the_table', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'ignore the_table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, dolt_diff_type FROM dolt_diff('HEAD~2', 'HEAD~1', 'the_table') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("added")],
                            &[T("2"), T("20"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_diff_stat_table_function_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "invalid arguments",
            intent: "dolt_diff_stat refuses wrong argument counts, NULLs, missing tables and revisions, and arguments it can't evaluate before the query runs.",
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
                    query: "SELECT * FROM dolt_diff_stat();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff_stat() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('t');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff_stat' expected 2 or 3 arguments, 1 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('t', current_setting('test.Commit1'), current_setting('test.Commit2'), 'extra');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff_stat' expected 2 or 3 arguments, 4 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(NULL, NULL, NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_diff_stat: NULL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(123, current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff_stat(integer, text, text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('t', 123, current_setting('test.Commit2'));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff_stat(unknown, integer, text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('t', current_setting('test.Commit1'), 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff_stat(unknown, text, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('fake-branch', current_setting('test.Commit2'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('fake-branch..main', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), 'fake-branch', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main..fake-branch', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit2'), 'doesnotexist');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main^..main', 'doesnotexist');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), concat('fake', '-', 'branch'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(dolt_hashof('main'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit2'), LOWER('T'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("2"), T("0"), T("0"), T("6"), T("0"), T("0"), T("0"), T("2"), T("0"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main..main~', LOWER('T'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("2"), T("0"), T("0"), T("6"), T("0"), T("2"), T("0"), T("6"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with single table",
            intent: "dolt_diff_stat counts a table's added, deleted, and modified rows and cells between two commits, in either direction.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('--allow-empty', '-m', 'creating table t'), FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting 1 into table t'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four');",
                "UPDATE t SET c1='uno', c2='dos' WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting 2 into table t'), FALSE);",
                "DROP TABLE t;",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'drop table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("0"), T("0"), T("3"), T("0"), T("0"), T("0"), T("1"), T("0"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("2"), T("0"), T("1"), T("6"), T("0"), T("2"), T("1"), T("3"), T("3"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit4'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("2"), T("1"), T("0"), T("6"), T("2"), T("3"), T("1"), T("9"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("3"), T("0"), T("0"), T("9"), T("0"), T("3"), T("0"), T("9"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("3"), T("0"), T("0"), T("9"), T("0"), T("0"), T("0"), T("3"), T("0"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "t" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT *\nFROM dolt_diff_stat(current_setting('test.Commit3'), current_setting('test.Commit4'), 't') \nINNER JOIN t AS of current_setting('test.Commit3') ON rows_unmodified = t.pk;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8), Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
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
            name: "basic case with single keyless table",
            intent: "dolt_diff_stat counts a keyless table's changes, an update being a deleted and an added row.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('--allow-empty', '-m', 'creating table t'), FALSE);",
                "CREATE TABLE t (id int, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting 1 into table t'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four');",
                "UPDATE t SET c1='uno', c2='dos' WHERE id=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting 2 into table t'), FALSE);",
                "DROP TABLE t;",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'drop table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1"), T("0"), Null, Null, Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("3"), T("1"), Null, Null, Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit4'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1"), T("3"), Null, Null, Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("0"), T("3"), Null, Null, Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("3"), T("0"), Null, Null, Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "t" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with multiple tables",
            intent: "dolt_diff_stat without a table counts every table that changed, one row each.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "CREATE TABLE t2 (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t2 VALUES(100, 'hundred', 'hundert');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into table t2'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four'), (4, 'four', 'five');",
                "UPDATE t SET c1='uno', c2='dos' WHERE pk=1;",
                "INSERT INTO t2 VALUES(101, 'hundred one', 'one');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "DELETE FROM t WHERE c2 = 'four';",
                "UPDATE t2 SET c2='zero' WHERE pk=100;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "CREATE TABLE keyless (id int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit0'), current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("0"), T("0"), T("3"), T("0"), T("0"), T("0"), T("1"), T("0"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t2"), T("0"), T("1"), T("0"), T("0"), T("3"), T("0"), T("0"), T("0"), T("1"), T("0"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit2'), current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("3"), T("0"), T("1"), T("9"), T("0"), T("2"), T("1"), T("4"), T("3"), T("12")],
                            &[T("public.t2"), T("1"), T("1"), T("0"), T("0"), T("3"), T("0"), T("0"), T("1"), T("2"), T("3"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit3'), current_setting('test.Commit4'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("3"), T("0"), T("1"), T("0"), T("0"), T("3"), T("0"), T("4"), T("3"), T("12"), T("9")],
                            &[T("public.t2"), T("1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("1"), T("2"), T("2"), T("6"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit4'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("2"), T("1"), T("0"), T("6"), T("2"), T("3"), T("1"), T("9"), T("3")],
                            &[T("public.t2"), T("0"), T("0"), T("1"), T("1"), T("0"), T("3"), T("1"), T("2"), T("1"), T("6"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit3'), 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("3"), T("0"), T("1"), T("0"), T("0"), T("3"), T("0"), T("4"), T("3"), T("12"), T("9")],
                            &[T("public.t2"), T("1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("1"), T("2"), T("2"), T("6"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WORKING and STAGED",
            intent: "dolt_diff_stat reads WORKING and STAGED as revisions, and staging moves the changes between them.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text, c2 text);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'three', 'four');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'inserting two rows into table t'), FALSE);",
                "INSERT INTO t VALUES (3, 'five', 'six');",
                "DELETE FROM t WHERE pk = 2;",
                "UPDATE t SET c2 = '100' WHERE pk = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("1"), T("3"), T("3"), T("1"), T("2"), T("2"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('STAGED', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("1"), T("3"), T("3"), T("1"), T("2"), T("2"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('STAGED..WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("1"), T("3"), T("3"), T("1"), T("2"), T("2"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('WORKING', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("1"), T("3"), T("3"), T("1"), T("2"), T("2"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('WORKING', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('WORKING..WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('STAGED', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('WORKING', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("1"), T("3"), T("3"), T("1"), T("2"), T("2"), T("6"), T("6")],
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
            name: "diff with branch refs",
            intent: "dolt_diff_stat takes branches, ancestry specs, two-dot ranges, and three-dot ranges from the merge base.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting row 1 into t in main'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'dropping column c2 in branch1'), FALSE);",
                "DELETE FROM t WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'deleting row 1 in branch1'), FALSE);",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'inserting row 2 in branch1'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'two', 'three');",
                "SELECT set_config('test.Commit6', ('')::text, FALSE);",
                "SELECT set_config('test.Commit6', dolt_commit('-am', 'inserting row 2 in main'), FALSE);",
                "CREATE TABLE newtable (pk int PRIMARY KEY);",
                "INSERT INTO newtable VALUES (1), (2);",
                "SELECT set_config('test.Commit7', ('')::text, FALSE);",
                "SELECT set_config('test.Commit7', dolt_commit('-Am', 'new table newtable'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("1"), T("1"), T("0"), T("4"), T("0"), T("2"), T("1"), T("6"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main..branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("1"), T("1"), T("0"), T("4"), T("0"), T("2"), T("1"), T("6"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("1"), T("1"), T("0"), T("4"), T("0"), T("2"), T("1"), T("6"), T("2")],
                            &[T("public.newtable"), T("0"), T("0"), T("2"), T("0"), T("0"), T("2"), T("0"), T("2"), T("0"), T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main..branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("1"), T("1"), T("0"), T("4"), T("0"), T("2"), T("1"), T("6"), T("2")],
                            &[T("public.newtable"), T("0"), T("0"), T("2"), T("0"), T("0"), T("2"), T("0"), T("2"), T("0"), T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('branch1', 'main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("0"), T("1"), T("4"), T("0"), T("1"), T("1"), T("2"), T("2"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('branch1..main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("0"), T("1"), T("4"), T("0"), T("1"), T("1"), T("2"), T("2"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~2', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("0"), T("2"), T("3"), T("0"), T("1"), T("1"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main~2..branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("0"), T("2"), T("3"), T("0"), T("1"), T("1"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main...branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("0"), T("2"), T("3"), T("0"), T("1"), T("1"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main...branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("1"), T("0"), T("2"), T("3"), T("0"), T("1"), T("1"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('branch1...main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("1"), T("0"), T("0"), T("3"), T("0"), T("0"), T("1"), T("2"), T("3"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('branch1...main');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("1"), T("0"), T("0"), T("3"), T("0"), T("0"), T("1"), T("2"), T("3"), T("6")],
                            &[T("public.newtable"), T("0"), T("2"), T("0"), T("0"), T("2"), T("0"), T("0"), T("0"), T("2"), T("0"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('branch1...main^');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("1"), T("0"), T("0"), T("3"), T("0"), T("0"), T("1"), T("2"), T("3"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('branch1...main', 'newtable');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.newtable"), T("0"), T("2"), T("0"), T("0"), T("2"), T("0"), T("0"), T("0"), T("2"), T("0"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main...main', 'newtable');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
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
            name: "schema modification: drop and add column",
            intent: "dolt_diff_stat counts cells of the columns each side has when a column is dropped and added again.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'inserting row 1, 2 into t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c2'), FALSE);",
                "INSERT INTO t VALUES (3, 'three');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting row 3'), FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(20);",
                "INSERT INTO t VALUES (4, 'four', 'five');",
                "UPDATE t SET c2='foo' WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'adding column c2, inserting, and updating data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("0"), T("2"), T("0"), T("2"), T("0"), T("2"), T("2"), T("6"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("2"), T("1"), T("0"), T("0"), T("2"), T("0"), T("0"), T("2"), T("3"), T("4"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("1"), T("0"), T("2"), T("2"), T("2"), T("0"), T("2"), T("3"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("2"), T("1"), T("0"), T("1"), T("6"), T("0"), T("1"), T("3"), T("4"), T("6"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("2"), T("0"), T("2"), T("6"), T("0"), T("2"), T("2"), T("4"), T("6"), T("12")],
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
            name: "schema modification: rename columns",
            intent: "dolt_diff_stat counts a renamed column's cells as the same column.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 int);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', -1), (2, 'two', -2);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c2 TO c3;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'renaming column c2 to c3'), FALSE);",
                "INSERT INTO t VALUES (3, 'three', -3);",
                "UPDATE t SET c3=1 WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting and updating data'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c3 TO c2;",
                "INSERT INTO t VALUES (4, 'four', -4);",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'renaming column c3 to c2, and inserting data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("2"), T("0"), T("0"), T("6"), T("0"), T("0"), T("0"), T("2"), T("0"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("1"), T("0"), T("1"), T("3"), T("0"), T("1"), T("2"), T("3"), T("6"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("3"), T("1"), T("0"), T("0"), T("3"), T("0"), T("0"), T("3"), T("4"), T("9"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("4"), T("0"), T("0"), T("12"), T("0"), T("0"), T("0"), T("4"), T("0"), T("12")],
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
            name: "new table",
            intent: "dolt_diff_stat counts a new table's rows as added, and as deleted in the other direction.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('WORKING', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1,2);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("0"), T("1"), T("0"), T("0"), T("2"), T("0"), T("0"), T("0"), T("1"), T("0"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('WORKING', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("2"), T("0"), T("1"), T("0"), T("2"), T("0")],
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
            name: "dropped table",
            intent: "dolt_diff_stat counts a dropped table's rows as deleted, and as added in the other direction.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2);",
                "SELECT dolt_commit('-am', 'new table');",
                "DROP TABLE t1;",
                "SELECT dolt_commit('-am', 'dropped table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("2"), T("0"), T("1"), T("0"), T("2"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'HEAD~', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("0"), T("1"), T("0"), T("0"), T("2"), T("0"), T("0"), T("0"), T("1"), T("0"), T("2")],
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
            name: "renamed table",
            intent: "dolt_diff_stat follows a renamed table by either name.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2);",
                "SELECT dolt_commit('-am', 'new table');",
                "ALTER TABLE t1 RENAME TO t2;",
                "SELECT dolt_add('.');",
                "INSERT INTO t2 VALUES (3,4);",
                "SELECT dolt_commit('-am', 'renamed table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~', 'HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t2"), T("1"), T("1"), T("0"), T("0"), T("2"), T("0"), T("0"), T("1"), T("2"), T("2"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~..HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t2"), T("1"), T("1"), T("0"), T("0"), T("2"), T("0"), T("0"), T("1"), T("2"), T("2"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("1"), T("1"), T("0"), T("0"), T("2"), T("0"), T("0"), T("1"), T("2"), T("2"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~..HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("1"), T("1"), T("0"), T("0"), T("2"), T("0"), T("0"), T("1"), T("2"), T("2"), T("4")],
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
            name: "add multiple columns, then set and unset a value. Should not show a diff",
            intent: "Setting a value and setting it back leaves no changed rows in dolt_diff_stat.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'add columns');",
                "UPDATE t SET col1 = 1 WHERE pk = 1;",
                "UPDATE t SET col1 = NULL WHERE pk = 1;",
                "SELECT dolt_commit('--allow-empty', '-am', 'fix short tuple');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~2', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0"), T("0"), T("0"), T("2"), T("0"), T("0"), T("1"), T("1"), T("1"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
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
            name: "pk set change should throw an error for 3 argument dolt_diff_stat",
            intent: "dolt_diff_stat of one table fails when that table's primary key changed.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'table with row');",
                "ALTER TABLE t ADD col1 int NOT NULL DEFAULT 0;",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "ALTER TABLE t ADD PRIMARY KEY (pk, col1);",
                "SELECT dolt_commit('-am', 'add secondary column with primary key');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~', 'HEAD', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("0"), T("1"), T("1"), T("0"), T("0"), T("1"), T("1"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pk set change should report warning for 2 argument dolt_diff_stat",
            intent: "dolt_diff_stat of every table warns about a table whose primary key changed and counts the others.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "INSERT INTO t2 VALUES (2);",
                "SELECT dolt_commit('-Am', 'multiple tables');",
                "ALTER TABLE t ADD col1 int NOT NULL DEFAULT 0;",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "ALTER TABLE t ADD PRIMARY KEY (pk, col1);",
                "INSERT INTO t2 VALUES (3), (4), (5);",
                "SELECT dolt_commit('-am', 'add secondary column with primary key to t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD~', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t"), T("0"), T("0"), T("0"), T("1"), T("1"), T("0"), T("0"), T("1"), T("1"), T("1"), T("2")],
                            &[T("public.t2"), T("1"), T("3"), T("0"), T("0"), T("3"), T("0"), T("0"), T("1"), T("4"), T("1"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table drop and recreate with identical primary key reports modified row in diff stat",
            intent: "A table dropped and made again with the same key counts a changed row as modified, not deleted and added.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY, name varchar(10));",
                "INSERT INTO t1 VALUES (1, 'dolt');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'create_table'), FALSE);",
                "UPDATE t1 SET name = 'doltgres' WHERE id = 1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'update_value'), FALSE);",
                "CREATE TABLE temp__t1 (id int PRIMARY KEY, name varchar(10));",
                "INSERT INTO temp__t1 VALUES (1, 'dolt');",
                "DROP TABLE t1;",
                "ALTER TABLE temp__t1 RENAME TO t1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-Am', 'drop and recreate'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT rows_modified, cells_modified, old_row_count, new_row_count FROM dolt_diff_stat(current_setting('test.Commit2'), current_setting('test.Commit3'), 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("rows_modified", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("1")],
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
fn test_diff_summary_table_function_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "invalid arguments",
            intent: "dolt_diff_summary refuses wrong argument counts, NULLs, missing revisions, and arguments it can't evaluate before the query runs.",
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
                    query: "SELECT * FROM dolt_diff_summary();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff_summary() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('t');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff_summary' expected 2 or 3 arguments, 1 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('t', current_setting('test.Commit1'), current_setting('test.Commit2'), 'extra');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_diff_summary' expected 2 or 3 arguments, 4 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(NULL, NULL, NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_diff_summary: NULL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(123, current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff_summary(integer, text, text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('t', 123, current_setting('test.Commit2'));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff_summary(unknown, integer, text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('t', current_setting('test.Commit1'), 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_diff_summary(unknown, text, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('fake-branch', current_setting('test.Commit2'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('fake-branch..main', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), 'fake-branch', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main..fake-branch', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), concat('fake', '-', 'branch'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(dolt_hashof('main'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit2'), LOWER('T'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with single table",
            intent: "dolt_diff_summary reports whether a table's data or schema changed between two commits, or that it was added or dropped.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('--allow-empty', '-m', 'creating table t'), FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting 1 into table t'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four');",
                "UPDATE t SET c1='uno', c2='dos' WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting 2 into table t'), FALSE);",
                "DROP TABLE t;",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'drop table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit2'), 'doesnotexist');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit4'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T(""), T("dropped"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
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
            name: "basic case with single keyless table",
            intent: "dolt_diff_summary reports a keyless table's changes as a primary keyed one's.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('--allow-empty', '-m', 'creating table t'), FALSE);",
                "CREATE TABLE t (id int, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting 1 into table t'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four');",
                "UPDATE t SET c1='uno', c2='dos' WHERE id=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting 2 into table t'), FALSE);",
                "DROP TABLE t;",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'drop table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit4'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T(""), T("dropped"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
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
            name: "basic case with multiple tables",
            intent: "dolt_diff_summary without a table reports every table that changed.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "CREATE TABLE t2 (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t2 VALUES(100, 'hundred', 'hundert');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into table t2'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four'), (4, 'four', 'five');",
                "UPDATE t SET c1='uno', c2='dos' WHERE pk=1;",
                "INSERT INTO t2 VALUES(101, 'hundred one', 'one');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "DELETE FROM t WHERE c2 = 'four';",
                "UPDATE t2 SET c2='zero' WHERE pk=100;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "CREATE TABLE keyless (id int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit0'), current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t2"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit2'), current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                            &[T("public.t2"), T("public.t2"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit3'), current_setting('test.Commit4'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                            &[T("public.t2"), T("public.t2"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit0'), current_setting('test.Commit4'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t"), T("added"), T("t"), T("t")],
                            &[T(""), T("public.t2"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit4'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                            &[T("public.t2"), T("public.t2"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit3'), 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                            &[T("public.t2"), T("public.t2"), T("modified"), T("t"), T("f")],
                            &[T(""), T("public.keyless"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WORKING and STAGED",
            intent: "dolt_diff_summary reads WORKING and STAGED as revisions, and staging moves the changes between them.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text, c2 text);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'three', 'four');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'inserting two rows into table t'), FALSE);",
                "INSERT INTO t VALUES (3, 'five', 'six');",
                "DELETE FROM t WHERE pk = 2;",
                "UPDATE t SET c2 = '100' WHERE pk = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('STAGED', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('STAGED..WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('WORKING', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('WORKING', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('WORKING..WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('STAGED', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('WORKING', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
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
            name: "diff with branch refs",
            intent: "dolt_diff_summary takes branches, ancestry specs, two-dot ranges, and three-dot ranges from the merge base.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting row 1 into t in main'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'dropping column c2 in branch1'), FALSE);",
                "DELETE FROM t WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'deleting row 1 in branch1'), FALSE);",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'inserting row 2 in branch1'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'two', 'three');",
                "SELECT set_config('test.Commit6', ('')::text, FALSE);",
                "SELECT set_config('test.Commit6', dolt_commit('-am', 'inserting row 2 in main'), FALSE);",
                "CREATE TABLE newtable (pk int PRIMARY KEY);",
                "INSERT INTO newtable VALUES (1), (2);",
                "SELECT set_config('test.Commit7', ('')::text, FALSE);",
                "SELECT set_config('test.Commit7', dolt_commit('-Am', 'new table newtable'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main..branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                            &[T("public.newtable"), T(""), T("dropped"), T("t"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main..branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                            &[T("public.newtable"), T(""), T("dropped"), T("t"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('branch1', 'main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('branch1..main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~2', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main~2..branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main...branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main...branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('branch1...main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('branch1...main');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                            &[T(""), T("public.newtable"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('branch1...main^');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('branch1...main', 'newtable');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.newtable"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main...main', 'newtable');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
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
            name: "schema modification: drop and add column",
            intent: "Dropping and adding a column are schema changes in dolt_diff_summary, and writing rows a data change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'inserting row 1, 2 into t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c2'), FALSE);",
                "INSERT INTO t VALUES (3, 'three');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting row 3'), FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(20);",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'adding column c2'), FALSE);",
                "INSERT INTO t VALUES (4, 'four', 'five');",
                "UPDATE t SET c2='foo' WHERE pk=1;",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'inserting and updating data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit3'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
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
            name: "schema modification: rename columns",
            intent: "Renaming a column is a schema change in dolt_diff_summary.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 int);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', -1), (2, 'two', -2);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into t'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c2 TO c3;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'renaming column c2 to c3'), FALSE);",
                "INSERT INTO t VALUES (3, 'three', -3);",
                "UPDATE t SET c3=1 WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting and updating data'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c3 TO c2;",
                "INSERT INTO t VALUES (4, 'four', -4);",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'renaming column c3 to c2, and inserting data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("f")],
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
            name: "new table",
            intent: "dolt_diff_summary reports a new table as added, and as dropped in the other direction.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t1"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('WORKING', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T(""), T("dropped"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1,2);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t1"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('WORKING', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T(""), T("dropped"), T("t"), T("t")],
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
            name: "dropped table",
            intent: "dolt_diff_summary reports a dropped table as dropped, and as added in the other direction.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2);",
                "SELECT dolt_commit('-am', 'new table');",
                "DROP TABLE t1;",
                "SELECT dolt_commit('-am', 'dropped table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T(""), T("dropped"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'HEAD~', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t1"), T("added"), T("t"), T("t")],
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
            name: "renamed table",
            intent: "dolt_diff_summary reports a renamed table under both names, by either name.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2);",
                "SELECT dolt_commit('-am', 'new table');",
                "ALTER TABLE t1 RENAME TO t2;",
                "SELECT dolt_add('.');",
                "INSERT INTO t2 VALUES (3,4);",
                "SELECT dolt_commit('-am', 'renamed table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~', 'HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T("public.t2"), T("renamed"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~..HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T("public.t2"), T("renamed"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T("public.t2"), T("renamed"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~..HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T("public.t2"), T("renamed"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T("public.t2"), T("renamed"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~..HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T("public.t2"), T("renamed"), T("t"), T("t")],
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
            name: "foreign key change",
            intent: "Adding or dropping a foreign key is a schema change in dolt_diff_summary.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'create table test'), FALSE);",
                "CREATE TABLE test2 (pk int PRIMARY KEY, test_id int);",
                "ALTER TABLE test2 ADD CONSTRAINT fk_test_id FOREIGN KEY (test_id) REFERENCES test(id);",
                "INSERT INTO test2 VALUES (1, 1);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'table with foreign key and row'), FALSE);",
                "ALTER TABLE test2 DROP CONSTRAINT fk_test_id;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.test2"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test2"), T("public.test2"), T("modified"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary(current_setting('test.Commit1'), 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.test2"), T("added"), T("t"), T("t")],
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
            name: "add multiple columns, then set and unset a value. Should not show a diff",
            intent: "Setting a value and setting it back leaves no data change in dolt_diff_summary.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'add columns');",
                "UPDATE t SET col1 = 1 WHERE pk = 1;",
                "UPDATE t SET col1 = NULL WHERE pk = 1;",
                "SELECT dolt_commit('--allow-empty', '-am', 'fix short tuple');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~2', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
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
            name: "pk set change should throw an error for 3 argument dolt_diff_summary",
            intent: "dolt_diff_summary of one table fails when that table's primary key changed.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'table with row');",
                "ALTER TABLE t ADD col1 int NOT NULL DEFAULT 0;",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "ALTER TABLE t ADD PRIMARY KEY (pk, col1);",
                "SELECT dolt_commit('-am', 'add secondary column with primary key');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~', 'HEAD', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pk set change should report warning for 2 argument dolt_diff_summary",
            intent: "dolt_diff_summary of every table warns about a table whose primary key changed and reports the others.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "INSERT INTO t2 VALUES (2);",
                "SELECT dolt_commit('-Am', 'multiple tables');",
                "ALTER TABLE t ADD col1 int NOT NULL DEFAULT 0;",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "ALTER TABLE t ADD PRIMARY KEY (pk, col1);",
                "INSERT INTO t2 VALUES (3), (4), (5);",
                "SELECT dolt_commit('-am', 'add secondary column with primary key to t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("t"), T("t")],
                            &[T("public.t2"), T("public.t2"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "raising the autoincrement value should be seen as a schema change",
            intent: "Advancing a table's identity sequence shows in dolt_diff_summary even when no row changed.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY);",
                "INSERT INTO t DEFAULT VALUES;",
                "SELECT dolt_commit('-Am', 'first commit');",
                "INSERT INTO t DEFAULT VALUES;",
                "DELETE FROM t WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'second commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("public.t"), T("modified"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table drop and recreate with identical primary key reports modified data in diff summary",
            intent: "A table dropped and made again with the same key is a modified table with a data change.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY, name varchar(10));",
                "INSERT INTO t1 VALUES (1, 'dolt');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'create_table'), FALSE);",
                "UPDATE t1 SET name = 'doltgres' WHERE id = 1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'update_value'), FALSE);",
                "CREATE TABLE temp__t1 (id int PRIMARY KEY, name varchar(10));",
                "INSERT INTO temp__t1 VALUES (1, 'dolt');",
                "DROP TABLE t1;",
                "ALTER TABLE temp__t1 RENAME TO t1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-Am', 'drop and recreate'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT diff_type, data_change FROM dolt_diff_summary(current_setting('test.Commit2'), current_setting('test.Commit3'), 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("diff_type", TEXT), Column("data_change", BOOL)],
                        rows: &[
                            &[T("modified"), T("t")],
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
fn test_patch_table_function_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "invalid arguments",
            intent: "dolt_patch refuses wrong argument counts, NULLs, missing tables and revisions, and arguments it can't evaluate before the query runs.",
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
                    query: "SELECT * FROM dolt_patch();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_patch() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('t');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_patch' expected 2 or 3 arguments, 1 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('t', current_setting('test.Commit1'), current_setting('test.Commit2'), 'extra');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_patch' expected 2 or 3 arguments, 4 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(NULL, NULL, NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_patch: NULL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(123, current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_patch(integer, text, text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('t', 123, current_setting('test.Commit2'));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_patch(unknown, integer, text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('t', current_setting('test.Commit1'), 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_patch(unknown, text, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('fake-branch', current_setting('test.Commit2'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('fake-branch..main', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(current_setting('test.Commit1'), 'fake-branch', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main..fake-branch', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(current_setting('test.Commit1'), current_setting('test.Commit2'), 'doesnotexist');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main^..main', 'doesnotexist');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(current_setting('test.Commit1'), concat('fake', '-', 'branch'), 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(dolt_hashof('main'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(current_setting('test.Commit1'), current_setting('test.Commit2'), LOWER('T'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), Any, Any, T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (1, 'one', 'two');")],
                            &[T("2"), Any, Any, T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (2, 'two', 'three');")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main..main~', LOWER('T'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), Any, Any, T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                            &[T("2"), Any, Any, T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 2;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with single table",
            intent: "dolt_patch writes the statements that turn one commit's table into another's: its creation, inserts, updates, deletes, and its drop.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('--allow-empty', '-m', 'creating table t'), FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting 1 into table t'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four');",
                "UPDATE t SET c1='uno', c2='dos' WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting 2 into table t'), FALSE);",
                "DROP TABLE t;",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'drop table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(current_setting('test.Commit1'), current_setting('test.Commit2'), 't') WHERE diff_type = 'data';",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, statement FROM dolt_patch(current_setting('test.Commit1'), current_setting('test.Commit2'), 't') WHERE diff_type = 'schema';",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    c1 character varying(20),\n    c2 character varying(20)\n);")],
                            &[T("2"), T("public.t"), T("ALTER TABLE public.t OWNER TO postgres;")],
                            &[T("3"), T("public.t"), T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit2'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (1, 'one', 'two');")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit3'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("data"), T("UPDATE public.t SET c1 = 'uno', c2 = 'dos' WHERE pk = 1;")],
                            &[T("2"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (2, 'two', 'three');")],
                            &[T("3"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (3, 'three', 'four');")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit4'), current_setting('test.Commit3'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("data"), T("UPDATE public.t SET c1 = 'one', c2 = 'two' WHERE pk = 1;")],
                            &[T("2"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 2;")],
                            &[T("3"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 3;")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit4'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("DROP TABLE public.t;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit1'), current_setting('test.Commit4'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    c1 character varying(20),\n    c2 character varying(20)\n);")],
                            &[T("2"), T("public.t"), T("schema"), T("ALTER TABLE public.t OWNER TO postgres;")],
                            &[T("3"), T("public.t"), T("schema"), T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);")],
                            &[T("4"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (1, 'uno', 'dos');")],
                            &[T("5"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (2, 'two', 'three');")],
                            &[T("6"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (3, 'three', 'four');")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(current_setting('test.Commit1'), current_setting('test.Commit5'), 't');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "t" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "binary data in patch statements is hex encoded",
            intent: "dolt_patch writes binary keys and values as hex bytea literals.",
            set_up_script: &[
                "CREATE TABLE t (pk bytea PRIMARY KEY, c1  bytea);",
                "INSERT INTO t VALUES ('\\x42'::bytea, NULL);",
                "SELECT dolt_commit('-Am', 'new table with binary pk');",
                "UPDATE t SET c1 = '\\xeeee'::bytea WHERE pk = '\\x42'::bytea;",
                "INSERT INTO t VALUES ('\\x012345'::bytea, NULL), ('\\x054321'::bytea, convert_to('efg_!4', 'UTF8'));",
                "SELECT dolt_commit('-am', 'more rows');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~', 'HEAD', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("INSERT INTO public.t (pk, c1) VALUES ('\\x012345', NULL);")],
                            &[T("INSERT INTO public.t (pk, c1) VALUES ('\\x054321', '\\x6566675f2134');")],
                            &[T("UPDATE public.t SET c1 = '\\xeeee' WHERE pk = '\\x42';")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("binary(16) is MySQL's and pads its values; bytea stands for it and keeps them as written")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with multiple tables",
            intent: "dolt_patch without a table writes the statements for every table that changed, table by table.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "CREATE TABLE t2 (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t2 VALUES(100, 'hundred', 'hundert');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'inserting into table t2'), FALSE);",
                "INSERT INTO t VALUES(2, 'two', 'three'), (3, 'three', 'four'), (4, 'four', 'five');",
                "UPDATE t SET c1='uno', c2='dos' WHERE pk=1;",
                "INSERT INTO t2 VALUES(101, 'hundred one', 'one');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "DELETE FROM t WHERE c2 = 'four';",
                "UPDATE t2 SET c2='zero' WHERE pk=100;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'inserting into table t'), FALSE);",
                "CREATE TABLE keyless (id int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit0'), current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    c1 character varying(20),\n    c2 character varying(20)\n);")],
                            &[T("2"), T("public.t"), T("schema"), T("ALTER TABLE public.t OWNER TO postgres;")],
                            &[T("3"), T("public.t"), T("schema"), T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);")],
                            &[T("4"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (1, 'one', 'two');")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t2"), T("schema"), T("CREATE TABLE public.t2 (\n    pk integer NOT NULL,\n    c1 character varying(20),\n    c2 character varying(20)\n);")],
                            &[T("2"), T("public.t2"), T("schema"), T("ALTER TABLE public.t2 OWNER TO postgres;")],
                            &[T("3"), T("public.t2"), T("schema"), T("ALTER TABLE ONLY public.t2\n    ADD CONSTRAINT t2_pkey PRIMARY KEY (pk);")],
                            &[T("4"), T("public.t2"), T("data"), T("INSERT INTO public.t2 (pk, c1, c2) VALUES (100, 'hundred', 'hundert');")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit2'), current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("data"), T("UPDATE public.t SET c1 = 'uno', c2 = 'dos' WHERE pk = 1;")],
                            &[T("2"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (2, 'two', 'three');")],
                            &[T("3"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (3, 'three', 'four');")],
                            &[T("4"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (4, 'four', 'five');")],
                            &[T("5"), T("public.t2"), T("data"), T("INSERT INTO public.t2 (pk, c1, c2) VALUES (101, 'hundred one', 'one');")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit3'), current_setting('test.Commit4'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 3;")],
                            &[T("2"), T("public.t2"), T("data"), T("UPDATE public.t2 SET c2 = 'zero' WHERE pk = 100;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit4'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("data"), T("UPDATE public.t SET c1 = 'one', c2 = 'two' WHERE pk = 1;")],
                            &[T("2"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 2;")],
                            &[T("3"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 4;")],
                            &[T("4"), T("public.t2"), T("data"), T("UPDATE public.t2 SET c2 = 'hundert' WHERE pk = 100;")],
                            &[T("5"), T("public.t2"), T("data"), T("DELETE FROM public.t2 WHERE pk = 101;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit3'), 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.keyless"), T("schema"), T("CREATE TABLE public.keyless (\n    id integer\n);")],
                            &[T("2"), T("public.keyless"), T("schema"), T("ALTER TABLE public.keyless OWNER TO postgres;")],
                            &[T("3"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 3;")],
                            &[T("4"), T("public.t2"), T("data"), T("UPDATE public.t2 SET c2 = 'zero' WHERE pk = 100;")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "using WORKING and STAGED refs on RENAME, DROP and ADD column",
            intent: "dolt_patch writes column renames, drops, and adds, then the rows they changed, between WORKING, STAGED, and HEAD.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int, c3 int, c4 int, c5 int); COMMENT ON COLUMN t.c5 IS 'tag:5';",
                "INSERT INTO t VALUES (0,1,2,3,4,5), (1,1,2,3,4,5);",
                "SELECT dolt_commit('-Am', 'inserting two rows into table t');",
                "ALTER TABLE t RENAME COLUMN c1 TO c0;",
                "ALTER TABLE t DROP COLUMN c4;",
                "ALTER TABLE t ADD c6 bigint;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t RENAME COLUMN c1 TO c0;")],
                            &[T("2"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c4;")],
                            &[T("3"), T("public.t"), T("schema"), T("ALTER TABLE public.t ADD COLUMN c6 bigint;")],
                            &[T("4"), T("public.t"), T("data"), T("UPDATE public.t SET c0 = 1 WHERE pk = 0;")],
                            &[T("5"), T("public.t"), T("data"), T("UPDATE public.t SET c0 = 1 WHERE pk = 1;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('STAGED', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("STAGED"), T("WORKING"), T("public.t"), T("schema"), T("ALTER TABLE public.t RENAME COLUMN c1 TO c0;")],
                            &[T("2"), T("STAGED"), T("WORKING"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c4;")],
                            &[T("3"), T("STAGED"), T("WORKING"), T("public.t"), T("schema"), T("ALTER TABLE public.t ADD COLUMN c6 bigint;")],
                            &[T("4"), T("STAGED"), T("WORKING"), T("public.t"), T("data"), T("UPDATE public.t SET c0 = 1 WHERE pk = 0;")],
                            &[T("5"), T("STAGED"), T("WORKING"), T("public.t"), T("data"), T("UPDATE public.t SET c0 = 1 WHERE pk = 1;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('STAGED..WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("STAGED"), T("WORKING"), T("public.t"), T("schema"), T("ALTER TABLE public.t RENAME COLUMN c1 TO c0;")],
                            &[T("2"), T("STAGED"), T("WORKING"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c4;")],
                            &[T("3"), T("STAGED"), T("WORKING"), T("public.t"), T("schema"), T("ALTER TABLE public.t ADD COLUMN c6 bigint;")],
                            &[T("4"), T("STAGED"), T("WORKING"), T("public.t"), T("data"), T("UPDATE public.t SET c0 = 1 WHERE pk = 0;")],
                            &[T("5"), T("STAGED"), T("WORKING"), T("public.t"), T("data"), T("UPDATE public.t SET c0 = 1 WHERE pk = 1;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('WORKING', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("WORKING"), T("STAGED"), T("public.t"), T("schema"), T("ALTER TABLE public.t RENAME COLUMN c0 TO c1;")],
                            &[T("2"), T("WORKING"), T("STAGED"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c6;")],
                            &[T("3"), T("WORKING"), T("STAGED"), T("public.t"), T("schema"), T("ALTER TABLE public.t ADD COLUMN c4 integer;")],
                            &[T("4"), T("WORKING"), T("STAGED"), T("public.t"), T("data"), T("UPDATE public.t SET c1 = 1, c4 = 4 WHERE pk = 0;")],
                            &[T("5"), T("WORKING"), T("STAGED"), T("public.t"), T("data"), T("UPDATE public.t SET c1 = 1, c4 = 4 WHERE pk = 1;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('WORKING', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('WORKING..WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('STAGED', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('WORKING', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'STAGED', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t RENAME COLUMN c1 TO c0;")],
                            &[T("2"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c4;")],
                            &[T("3"), T("public.t"), T("schema"), T("ALTER TABLE public.t ADD COLUMN c6 bigint;")],
                            &[T("4"), T("public.t"), T("data"), T("UPDATE public.t SET c0 = 1 WHERE pk = 0;")],
                            &[T("5"), T("public.t"), T("data"), T("UPDATE public.t SET c0 = 1 WHERE pk = 1;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "using branch refs different ways",
            intent: "dolt_patch takes branches, ancestry specs, two-dot ranges, and three-dot ranges from the merge base.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table t');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT dolt_commit('-am', 'inserting row 1 into t in main');",
                "SELECT set_config('test.Commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT dolt_commit('-am', 'dropping column c2 in branch1');",
                "SELECT set_config('test.Commit3', (dolt_hashof('HEAD'))::text, FALSE);",
                "DELETE FROM t WHERE pk=1;",
                "SELECT dolt_commit('-am', 'deleting row 1 in branch1');",
                "SELECT set_config('test.Commit4', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'inserting row 2 in branch1');",
                "SELECT set_config('test.Commit5', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'two', 'three');",
                "SELECT dolt_commit('-am', 'inserting row 2 in main');",
                "SELECT set_config('test.Commit6', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE newtable (pk int PRIMARY KEY);",
                "INSERT INTO newtable VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'new table newtable');",
                "SELECT set_config('test.Commit7', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c2;")],
                            &[T("2"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main..branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c2;")],
                            &[T("2"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.newtable"), T("schema"), T("DROP TABLE public.newtable;")],
                            &[T("2"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c2;")],
                            &[T("3"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main..branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.newtable"), T("schema"), T("DROP TABLE public.newtable;")],
                            &[T("2"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c2;")],
                            &[T("3"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('branch1', 'main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t ADD COLUMN c2 character varying(20);")],
                            &[T("2"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (1, 'one', 'two');")],
                            &[T("3"), T("public.t"), T("data"), T("UPDATE public.t SET c2 = 'three' WHERE pk = 2;")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('branch1..main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t ADD COLUMN c2 character varying(20);")],
                            &[T("2"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (1, 'one', 'two');")],
                            &[T("3"), T("public.t"), T("data"), T("UPDATE public.t SET c2 = 'three' WHERE pk = 2;")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main~2', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c2;")],
                            &[T("2"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                            &[T("3"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1) VALUES (2, 'two');")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main~2..branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c2;")],
                            &[T("2"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                            &[T("3"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1) VALUES (2, 'two');")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main...branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c2;")],
                            &[T("2"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                            &[T("3"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1) VALUES (2, 'two');")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main...branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("schema"), T("ALTER TABLE public.t DROP COLUMN c2;")],
                            &[T("2"), T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                            &[T("3"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1) VALUES (2, 'two');")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('branch1...main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (2, 'two', 'three');")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('branch1...main');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.newtable"), T("schema"), T("CREATE TABLE public.newtable (\n    pk integer NOT NULL\n);")],
                            &[T("2"), T("public.newtable"), T("schema"), T("ALTER TABLE public.newtable OWNER TO postgres;")],
                            &[T("3"), T("public.newtable"), T("schema"), T("ALTER TABLE ONLY public.newtable\n    ADD CONSTRAINT newtable_pkey PRIMARY KEY (pk);")],
                            &[T("4"), T("public.newtable"), T("data"), T("INSERT INTO public.newtable (pk) VALUES (1);")],
                            &[T("5"), T("public.newtable"), T("data"), T("INSERT INTO public.newtable (pk) VALUES (2);")],
                            &[T("6"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (2, 'two', 'three');")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('branch1...main^');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (2, 'two', 'three');")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('branch1...main', 'newtable');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.newtable"), T("schema"), T("CREATE TABLE public.newtable (\n    pk integer NOT NULL\n);")],
                            &[T("2"), T("public.newtable"), T("schema"), T("ALTER TABLE public.newtable OWNER TO postgres;")],
                            &[T("3"), T("public.newtable"), T("schema"), T("ALTER TABLE ONLY public.newtable\n    ADD CONSTRAINT newtable_pkey PRIMARY KEY (pk);")],
                            &[T("4"), T("public.newtable"), T("data"), T("INSERT INTO public.newtable (pk) VALUES (1);")],
                            &[T("5"), T("public.newtable"), T("data"), T("INSERT INTO public.newtable (pk) VALUES (2);")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('main...main', 'newtable');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "renamed table",
            intent: "dolt_patch writes a table's rename and then its changed rows, finding it by either name.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,2);",
                "SELECT dolt_commit('-am', 'new table');",
                "ALTER TABLE t1 RENAME TO t2;",
                "SELECT dolt_add('.');",
                "INSERT INTO t2 VALUES (3,4);",
                "SELECT dolt_commit('-am', 'renamed table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD~', 'HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t2"), T("schema"), T("ALTER TABLE public.t1 RENAME TO t2;")],
                            &[T("2"), T("public.t2"), T("data"), T("INSERT INTO public.t2 (a, b) VALUES (3, 4);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD~..HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t2"), T("schema"), T("ALTER TABLE public.t1 RENAME TO t2;")],
                            &[T("2"), T("public.t2"), T("data"), T("INSERT INTO public.t2 (a, b) VALUES (3, 4);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD~', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t2"), T("schema"), T("ALTER TABLE public.t1 RENAME TO t2;")],
                            &[T("2"), T("public.t2"), T("data"), T("INSERT INTO public.t2 (a, b) VALUES (3, 4);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD~..HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t2"), T("schema"), T("ALTER TABLE public.t1 RENAME TO t2;")],
                            &[T("2"), T("public.t2"), T("data"), T("INSERT INTO public.t2 (a, b) VALUES (3, 4);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multi PRIMARY KEY and FOREIGN KEY",
            intent: "dolt_patch writes foreign key and primary key changes, and warns that it leaves out the rows of a table whose key changed.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, id_ext int, v1 int, v2 text); COMMENT ON COLUMN parent.v2 IS 'tag:1'; CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int);",
                "SELECT dolt_commit('-Am', 'new tables');",
                "ALTER TABLE child ADD CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES parent(v1);",
                "INSERT INTO parent VALUES (0, 1, 2, NULL);",
                "ALTER TABLE parent DROP CONSTRAINT parent_pkey;",
                "ALTER TABLE parent ADD PRIMARY KEY(id, id_ext);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD~', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.child"), T("schema"), T("CREATE TABLE public.child (\n    id integer NOT NULL,\n    v1 integer\n);")],
                            &[T("2"), T("public.child"), T("schema"), T("ALTER TABLE public.child OWNER TO postgres;")],
                            &[T("3"), T("public.child"), T("schema"), T("ALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (id);")],
                            &[T("4"), T("public.parent"), T("schema"), T("CREATE TABLE public.parent (\n    id integer NOT NULL,\n    id_ext integer NOT NULL,\n    v1 integer,\n    v2 text\n);")],
                            &[T("5"), T("public.parent"), T("schema"), T("ALTER TABLE public.parent OWNER TO postgres;")],
                            &[T("6"), T("public.parent"), T("schema"), T("COMMENT ON COLUMN public.parent.v2 IS 'tag:1';")],
                            &[T("7"), T("public.parent"), T("schema"), T("ALTER TABLE ONLY public.parent\n    ADD CONSTRAINT parent_pkey PRIMARY KEY (id, id_ext);")],
                            &[T("8"), T("public.parent"), T("schema"), T("CREATE UNIQUE INDEX v1 ON public.parent USING btree (v1);")],
                            &[T("9"), T("public.parent"), T("data"), T("INSERT INTO public.parent (id, id_ext, v1, v2) VALUES (0, 1, 2, NULL);")],
                            &[T("10"), T("public.child"), T("schema"), T("ALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES public.parent(v1);")],
                        ],
                        tag: "SELECT 10",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, the column's comment included, and adds foreign keys after every table's statements and rows so the patch applies; a foreign key needs a unique index on the columns it references in Postgres, so parent's index on v1 is unique, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, to_commit_hash, table_name, diff_type, statement FROM dolt_patch('HEAD', 'STAGED');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("STAGED"), T("public.parent"), T("schema"), T("ALTER TABLE ONLY public.parent DROP CONSTRAINT parent_pkey;")],
                            &[T("2"), T("STAGED"), T("public.parent"), T("schema"), T("ALTER TABLE ONLY public.parent\n    ADD CONSTRAINT parent_pkey PRIMARY KEY (id, id_ext);")],
                            &[T("3"), T("STAGED"), T("public.parent"), T("data"), T("INSERT INTO public.parent (id, id_ext, v1, v2) VALUES (0, 1, 2, NULL);")],
                            &[T("4"), T("STAGED"), T("public.child"), T("schema"), T("ALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES public.parent(v1);")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("MySQL makes an index for each foreign key, which Postgres doesn't, so there is no index to add, and dolt_patch adds foreign keys after every table's statements so the patch applies, Appendix C \"dolt_patch statements\", and dolt_patch writes the rows of a table whose primary key changed, deleting the rows it changes by the old key before the definition changes and inserting the new rows after them, where Dolt warns and leaves them out, Appendix C \"dolt_patch of a table whose primary key changed\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement_order, to_commit_hash, table_name, diff_type, statement FROM dolt_patch('HEAD', 'STAGED');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("STAGED"), T("public.parent"), T("schema"), T("ALTER TABLE ONLY public.parent DROP CONSTRAINT parent_pkey;")],
                            &[T("2"), T("STAGED"), T("public.parent"), T("schema"), T("ALTER TABLE ONLY public.parent\n    ADD CONSTRAINT parent_pkey PRIMARY KEY (id, id_ext);")],
                            &[T("3"), T("STAGED"), T("public.parent"), T("data"), T("INSERT INTO public.parent (id, id_ext, v1, v2) VALUES (0, 1, 2, NULL);")],
                            &[T("4"), T("STAGED"), T("public.child"), T("schema"), T("ALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES public.parent(v1);")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW WARNINGS is MySQL's; Postgres sends each warning with the query that raised it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CHECK CONSTRAINTS",
            intent: "dolt_patch writes a new table's CHECK constraints in its creation.",
            set_up_script: &[
                "CREATE TABLE foo (pk int, c1 int, CHECK (c1 > 3), PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.foo"), T("schema"), T("CREATE TABLE public.foo (\n    pk integer NOT NULL,\n    c1 integer,\n    CONSTRAINT foo_c1_check CHECK ((c1 > 3))\n);")],
                            &[T("2"), T("public.foo"), T("schema"), T("ALTER TABLE public.foo OWNER TO postgres;")],
                            &[T("3"), T("public.foo"), T("schema"), T("ALTER TABLE ONLY public.foo\n    ADD CONSTRAINT foo_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "charset and collation changes",
            intent: "dolt_patch writes a table collation change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY) ;",
                "SELECT dolt_commit('-Am', 'empty table');",
                "SELECT set_config('test.commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (1);",
                "ALTER TABLE t ;",
                "SELECT dolt_commit('-am', 'inserting a row and altering the collation');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t 'utf8mb3';",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'inserting a row and altering the collation');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(current_setting('test.commit1'), current_setting('test.commit0'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), Any, Any, T("public.t"), T("schema"), T("ALTER TABLE `t` COLLATE='utf8mb4_0900_bin';")],
                            &[T("2"), Any, Any, T("public.t"), T("data"), T("DELETE FROM public.t WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch(current_setting('test.commit1'), current_setting('test.commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), Any, Any, T("public.t"), T("schema"), T("ALTER TABLE `t` COLLATE='utf8mb3_general_ci';")],
                            &[T("2"), Any, Any, T("public.t"), T("data"), T("INSERT INTO public.t (pk) VALUES (2);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: tables have no collation of their own in Postgres"),
            ..S
        },
        ScriptTest {
            name: "patch DDL changes",
            intent: "dolt_patch writes a column's type and comment change, a drop, a rename, and an add, then the rows they changed.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b int, c int);",
                "INSERT INTO t VALUES (1, NULL, 1, 1), (2, 2, NULL, 2), (3, 3, 3, 3);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER TABLE t ALTER COLUMN a TYPE varchar(100); COMMENT ON COLUMN t.a IS 'foo';",
                "ALTER TABLE t RENAME COLUMN c TO z;",
                "ALTER TABLE t DROP COLUMN b;",
                "ALTER TABLE t ADD COLUMN d int;",
                "DELETE FROM t WHERE pk = 3;",
                "UPDATE t SET a = 9 WHERE a IS NULL;",
                "INSERT INTO t VALUES (7,7,7,7);",
                "SELECT dolt_commit('-am', 'modified table t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('main', 'other', 't') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t ALTER COLUMN a TYPE character varying(100);")],
                            &[T("COMMENT ON COLUMN public.t.a IS 'foo';")],
                            &[T("ALTER TABLE public.t DROP COLUMN b;")],
                            &[T("ALTER TABLE public.t RENAME COLUMN c TO z;")],
                            &[T("ALTER TABLE public.t ADD COLUMN d integer;")],
                            &[T("UPDATE public.t SET a = '9', z = 1 WHERE pk = 1;")],
                            &[T("UPDATE public.t SET z = 2 WHERE pk = 2;")],
                            &[T("DELETE FROM public.t WHERE pk = 3;")],
                            &[T("INSERT INTO public.t (pk, a, z, d) VALUES (7, '7', 7, 7);")],
                        ],
                        tag: "SELECT 9",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "tag collision",
            intent: "Tables made on two branches show as dropped and created in a patch between them, whatever Dolt's column tags.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE \"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075_01\" (\"ID\" varchar(8) NOT NULL, \"PRODUCTO\" varchar(255), \"TARIFA\" int, \"VERSION_INICIO\" int, \"VERSION_FIN\" int, \"COBERTURA\" varchar(255), \"INDICADOR_NM\" varchar(255), \"ANOS_COMPANIA_PROCEDENCIA_MIN\" int, \"ANOS_COMPANIA_PROCEDENCIA_MAX\" int, \"COEFICIENTE\" double precision, \"DWB_IDENTITY\" varchar(64), PRIMARY KEY (\"ID\"))   ;",
                "CREATE TABLE \"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075\" (\"ID\" varchar(8) NOT NULL, \"PRODUCTO\" varchar(255), \"TARIFA\" int, \"VERSION_INICIO\" int, \"VERSION_FIN\" int, \"COBERTURA\" varchar(255), \"INDICADOR_NM\" varchar(255), \"ANOS_COMPANIA_PROCEDENCIA_MIN\" int, \"ANOS_COMPANIA_PROCEDENCIA_MAX\" int, \"COEFICIENTE\" double precision, \"DWB_IDENTITY\" varchar(64), PRIMARY KEY (\"ID\"))   ;",
                "SELECT dolt_commit('-A', '-m', 'create tables on other');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE \"MOTOR_TARIFA_COEFICIENTE_RIESGO_060\" (\"ID\" varchar(8) NOT NULL, \"PRODUCTO\" varchar(255), \"TARIFA\" int, \"VERSION_INICIO\" int, \"VERSION_FIN\" int, \"COBERTURA\" varchar(255), \"FRECUENCIA_PAGO\" varchar(255), \"COEFICIENTE\" double precision, \"DWB_IDENTITY\" varchar(64), PRIMARY KEY (\"ID\"))   ;",
                "CREATE TABLE \"MOTOR_TARIFA_COEFICIENTE_RIESGO_060_01\" (\"ID\" varchar(8) NOT NULL, \"PRODUCTO\" varchar(255), \"TARIFA\" int, \"VERSION_INICIO\" int, \"VERSION_FIN\" int, \"COBERTURA\" varchar(255), \"FRECUENCIA_PAGO\" varchar(255), \"COEFICIENTE\" double precision, \"DWB_IDENTITY\" varchar(64), PRIMARY KEY (\"ID\"))   ;",
                "SELECT dolt_commit('-A', '-m', 'create tables on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('main', 'other') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DROP TABLE public.\"MOTOR_TARIFA_COEFICIENTE_RIESGO_060\";")],
                            &[T("DROP TABLE public.\"MOTOR_TARIFA_COEFICIENTE_RIESGO_060_01\";")],
                            &[T("CREATE TABLE public.\"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075\" (\n    \"ID\" character varying(8) NOT NULL,\n    \"PRODUCTO\" character varying(255),\n    \"TARIFA\" integer,\n    \"VERSION_INICIO\" integer,\n    \"VERSION_FIN\" integer,\n    \"COBERTURA\" character varying(255),\n    \"INDICADOR_NM\" character varying(255),\n    \"ANOS_COMPANIA_PROCEDENCIA_MIN\" integer,\n    \"ANOS_COMPANIA_PROCEDENCIA_MAX\" integer,\n    \"COEFICIENTE\" double precision,\n    \"DWB_IDENTITY\" character varying(64)\n);")],
                            &[T("ALTER TABLE public.\"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075\" OWNER TO postgres;")],
                            &[T("ALTER TABLE ONLY public.\"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075\"\n    ADD CONSTRAINT \"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075_pkey\" PRIMARY KEY (\"ID\");")],
                            &[T("CREATE TABLE public.\"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075_01\" (\n    \"ID\" character varying(8) NOT NULL,\n    \"PRODUCTO\" character varying(255),\n    \"TARIFA\" integer,\n    \"VERSION_INICIO\" integer,\n    \"VERSION_FIN\" integer,\n    \"COBERTURA\" character varying(255),\n    \"INDICADOR_NM\" character varying(255),\n    \"ANOS_COMPANIA_PROCEDENCIA_MIN\" integer,\n    \"ANOS_COMPANIA_PROCEDENCIA_MAX\" integer,\n    \"COEFICIENTE\" double precision,\n    \"DWB_IDENTITY\" character varying(64)\n);")],
                            &[T("ALTER TABLE public.\"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075_01\" OWNER TO postgres;")],
                            &[T("ALTER TABLE ONLY public.\"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075_01\"\n    ADD CONSTRAINT \"MOTOR_TARIFA_COEFICIENTE_RIESGO_144_075_01_pkey\" PRIMARY KEY (\"ID\");")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_patch commit-to-commit shows tables added to dolt_ignore after the commit",
            intent: "Ignoring a table later doesn't hide its rows from a patch between earlier commits.",
            set_up_script: &[
                "CREATE TABLE the_table (pk int PRIMARY KEY, c1 int);",
                "INSERT INTO the_table VALUES (1, 10), (2, 20);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'add the_table');",
                "INSERT INTO dolt_ignore VALUES ('the_table', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'ignore the_table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT DISTINCT table_name FROM dolt_patch('HEAD~2', 'HEAD~1', 'the_table');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.the_table")],
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
            name: "table drop and recreate with identical primary key generates an UPDATE in diff patch",
            intent: "A table dropped and made again with the same key patches as an UPDATE of the changed row.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY, name varchar(10));",
                "INSERT INTO t1 VALUES (1, 'dolt');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'create_table'), FALSE);",
                "UPDATE t1 SET name = 'doltgres' WHERE id = 1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'update_value'), FALSE);",
                "CREATE TABLE temp__t1 (id int PRIMARY KEY, name varchar(10));",
                "INSERT INTO temp__t1 VALUES (1, 'dolt');",
                "DROP TABLE t1;",
                "ALTER TABLE temp__t1 RENAME TO t1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-Am', 'drop and recreate'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit2'), current_setting('test.Commit3'), 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t1"), T("schema"), T("ALTER TABLE ONLY public.t1 DROP CONSTRAINT t1_pkey;")],
                            &[T("2"), T("public.t1"), T("schema"), T("ALTER TABLE ONLY public.t1\n    ADD CONSTRAINT temp__t1_pkey PRIMARY KEY (id);")],
                            &[T("3"), T("public.t1"), T("data"), T("UPDATE public.t1 SET name = 'dolt' WHERE id = 1;")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_unscoped_diff_system_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "working set changes",
            intent: "The dolt_diff table has WORKING and STAGED rows for uncommitted changes, with NULL commit details.",
            set_up_script: &[
                "CREATE TABLE regularTable (a int PRIMARY KEY, b int, c int);",
                "CREATE TABLE droppedTable (a int PRIMARY KEY, b int, c int);",
                "CREATE TABLE renamedEmptyTable (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO regularTable VALUES (1, 2, 3), (2, 3, 4);",
                "INSERT INTO droppedTable VALUES (1, 2, 3), (2, 3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'Creating tables x and y'), FALSE);",
                "CREATE TABLE addedTable (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('addedTable');",
                "DROP TABLE droppedTable;",
                "SELECT dolt_add('droppedTable');",
                "ALTER TABLE renamedEmptyTable RENAME TO newRenamedEmptyTable;",
                "INSERT INTO regularTable VALUES (3, 4, 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF WHERE commit_hash = current_setting('test.Commit1');",
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
                    query: "SELECT * FROM DOLT_DIFF WHERE commit_hash = current_setting('test.Commit1') AND committer <> 'root';",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("committer", TEXT), Column("email", TEXT), Column("date", TIMESTAMP), Column("message", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL), Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMP)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, committer FROM DOLT_DIFF WHERE commit_hash <> current_setting('test.Commit1') AND committer = 'root' AND commit_hash NOT IN ('WORKING','STAGED');",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("committer", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name FROM DOLT_DIFF WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT)],
                        rows: &[
                            &[T("WORKING"), T("public.newrenamedemptytable")],
                            &[T("WORKING"), T("public.regulartable")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name FROM DOLT_DIFF WHERE commit_hash = 'STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT)],
                        rows: &[
                            &[T("STAGED"), T("public.addedtable")],
                            &[T("STAGED"), T("public.droppedtable")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name FROM DOLT_DIFF WHERE commit_hash <> current_setting('test.Commit1') AND commit_hash NOT IN ('STAGED') ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT)],
                        rows: &[
                            &[T("WORKING"), T("public.newrenamedemptytable")],
                            &[T("WORKING"), T("public.regulartable")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name FROM DOLT_DIFF WHERE commit_hash <> current_setting('test.Commit1') OR committer <> 'root' ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT)],
                        rows: &[
                            &[T("STAGED"), T("public.addedtable")],
                            &[T("STAGED"), T("public.droppedtable")],
                            &[T("WORKING"), T("public.newrenamedemptytable")],
                            &[T("WORKING"), T("public.regulartable")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_DIFF WHERE COMMIT_HASH IN ('WORKING', 'STAGED') ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("committer", TEXT), Column("email", TEXT), Column("date", TIMESTAMP), Column("message", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL), Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMP)],
                        rows: &[
                            &[T("STAGED"), T("public.addedtable"), Null, Null, Null, Null, T("f"), T("t"), Null, Null, Null],
                            &[T("STAGED"), T("public.droppedtable"), Null, Null, Null, Null, T("t"), T("t"), Null, Null, Null],
                            &[T("WORKING"), T("public.newrenamedemptytable"), Null, Null, Null, Null, T("f"), T("t"), Null, Null, Null],
                            &[T("WORKING"), T("public.regulartable"), Null, Null, Null, Null, T("t"), T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_add of a table dropped in the working set"),
            ..S
        },
        ScriptTest {
            name: "basic case with three tables",
            intent: "The dolt_diff table has a row per table each commit changed, telling schema changes from data changes.",
            set_up_script: &[
                "CREATE TABLE x (a int PRIMARY KEY, b int, c int);",
                "CREATE TABLE y (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO x VALUES (1, 2, 3), (2, 3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'Creating tables x and y'), FALSE);",
                "CREATE TABLE z (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO z VALUES (100, 101, 102);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'Creating tables z'), FALSE);",
                "INSERT INTO y VALUES (-1, -2, -3), (-2, -3, -4);",
                "INSERT INTO z VALUES (101, 102, 103);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'Inserting into tables y and z'), FALSE);",
                "ALTER TABLE y ADD COLUMN d int;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'Modify schema of table y'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF;",
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
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash = current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x"), T("t"), T("t")],
                            &[T("public.y"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.z"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.y"), T("f"), T("t")],
                            &[T("public.z"), T("f"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "renamed table",
            intent: "The dolt_diff table shows a renamed table under its new name in the commit that renamed it.",
            set_up_script: &[
                "CREATE TABLE x (a int PRIMARY KEY, b int, c int);",
                "CREATE TABLE y (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO x VALUES (1, 2, 3), (2, 3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'Creating tables x and y'), FALSE);",
                "CREATE TABLE z (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO z VALUES (100, 101, 102);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'Creating tables z'), FALSE);",
                "ALTER TABLE x RENAME TO x1;",
                "SELECT dolt_add('.');",
                "INSERT INTO x1 VALUES (1000, 1001, 1002);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'Renaming table x to x1 and inserting data'), FALSE);",
                "ALTER TABLE x1 RENAME TO x2;",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'Renaming table x1 to x2'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF;",
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
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x"), T("t"), T("t")],
                            &[T("public.y"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.z"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x1"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit4'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x2"), T("t"), T("f")],
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
            name: "dropped table",
            intent: "The dolt_diff table shows a dropped table in the commit that dropped it, with a data change when it had rows.",
            set_up_script: &[
                "CREATE TABLE x (a int PRIMARY KEY, b int, c int);",
                "CREATE TABLE y (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO x VALUES (1, 2, 3), (2, 3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'Creating tables x and y'), FALSE);",
                "DROP TABLE x;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'Dropping non-empty table x'), FALSE);",
                "DROP TABLE y;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'Dropping empty table y'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF;",
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
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x"), T("t"), T("t")],
                            &[T("public.y"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.y"), T("t"), T("f")],
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
            name: "empty commit handling",
            intent: "An empty commit has no rows in the dolt_diff table.",
            set_up_script: &[
                "CREATE TABLE x (a int PRIMARY KEY, b int, c int);",
                "CREATE TABLE y (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO x VALUES (1, 2, 3), (2, 3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'Creating tables x and y'), FALSE);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('--allow-empty', '-m', 'Empty!'), FALSE);",
                "INSERT INTO y VALUES (-1, -2, -3), (-2, -3, -4);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'Inserting into table y'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF;",
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
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x"), T("t"), T("t")],
                            &[T("public.y"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.y"), T("f"), T("t")],
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
            name: "includes commits from all branches",
            intent: "The dolt_diff table holds only HEAD's history, so commits on other branches are left out.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "CREATE TABLE x (a int PRIMARY KEY, b int, c int);",
                "CREATE TABLE y (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO x VALUES (1, 2, 3), (2, 3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'Creating tables x and y'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "CREATE TABLE z (a int PRIMARY KEY, b int, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO z VALUES (100, 101, 102);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'Creating tables z'), FALSE);",
                "INSERT INTO y VALUES (-1, -2, -3), (-2, -3, -4);",
                "INSERT INTO z VALUES (101, 102, 103);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'Inserting into tables y and z'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF;",
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
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x"), T("t"), T("t")],
                            &[T("public.y"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.z"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.y"), T("f"), T("t")],
                            &[T("public.z"), T("f"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge history handling",
            intent: "The dolt_diff table shows a merge commit's changes against its first parent.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "CREATE TABLE x (a int PRIMARY KEY, b int, c int);",
                "CREATE TABLE y (a int PRIMARY KEY, b int, c int);",
                "INSERT INTO x VALUES (1, 2, 3), (2, 3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-Am', 'Creating tables x and y'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branch2', 'HEAD~1');",
                "CREATE TABLE z (a int PRIMARY KEY, b int, c int);",
                "INSERT INTO z VALUES (100, 101, 102);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-Am', 'Creating tables z'), FALSE);",
                "SELECT dolt_merge('--no-commit', 'branch1');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'Merging branch1 into branch2'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF;",
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
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x"), T("t"), T("t")],
                            &[T("public.y"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.z"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, schema_change, data_change FROM DOLT_DIFF WHERE commit_hash IN (current_setting('test.Commit3')) ORDER by table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("schema_change", BOOL), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.x"), T("t"), T("t")],
                            &[T("public.y"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff committer columns when unset reflect --author",
            intent: "A commit made with --author shows the author as the dolt_diff table's committer too.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table', '--author', 'Test Author <author@example.com>');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'insert row', '--author', 'Test Author <author@example.com>');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT committer, email, author, author_email FROM dolt_diff WHERE message = 'insert row' LIMIT 1;",
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
    ]);
}

#[test]
fn test_column_diff_system_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "table changes - commit history",
            intent: "dolt_column_diff shows each column of modified, dropped, renamed, and added tables in a commit.",
            set_up_script: &[
                "CREATE TABLE modifiedTable (a int PRIMARY KEY, b int);",
                "INSERT INTO modifiedTable VALUES (1, 2), (2, 3);",
                "CREATE TABLE droppedTable (a int PRIMARY KEY, b int);",
                "INSERT INTO droppedTable VALUES (1, 2), (2, 3);",
                "CREATE TABLE renamedTable (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating tables');",
                "UPDATE modifiedTable SET b = 5 WHERE a = 1;",
                "DROP TABLE droppedTable;",
                "ALTER TABLE renamedTable RENAME TO newRenamedTable;",
                "CREATE TABLE addedTable (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'make table changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE table_name = 'public.modifiedtable';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.modifiedtable"), T("a"), T("added")],
                            &[T("public.modifiedtable"), T("b"), T("added")],
                            &[T("public.modifiedtable"), T("b"), T("modified")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE table_name = 'public.droppedtable';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.droppedtable"), T("a"), T("added")],
                            &[T("public.droppedtable"), T("b"), T("added")],
                            &[T("public.droppedtable"), T("a"), T("removed")],
                            &[T("public.droppedtable"), T("b"), T("removed")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE table_name = 'public.renamedtable' OR table_name = 'public.newrenamedtable';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.renamedtable"), T("a"), T("added")],
                            &[T("public.renamedtable"), T("b"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE table_name = 'public.addedtable';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.addedtable"), T("a"), T("added")],
                            &[T("public.addedtable"), T("b"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table changes - working set",
            intent: "dolt_column_diff shows each column of modified, dropped, renamed, and added tables as WORKING and STAGED changes.",
            set_up_script: &[
                "CREATE TABLE modifiedTable (a int PRIMARY KEY, b int);",
                "INSERT INTO modifiedTable VALUES (1, 2), (2, 3);",
                "CREATE TABLE droppedTable (a int PRIMARY KEY, b int);",
                "INSERT INTO droppedTable VALUES (1, 2), (2, 3);",
                "CREATE TABLE renamedTable (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "UPDATE modifiedTable SET b = 5 WHERE a = 1;",
                "DROP TABLE droppedTable;",
                "ALTER TABLE renamedTable RENAME TO newRenamedTable;",
                "CREATE TABLE addedTable (a int PRIMARY KEY, b int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE table_name = 'public.modifiedtable' ORDER BY commit_hash, table_name, column_name;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("STAGED"), T("public.modifiedtable"), T("a"), T("added")],
                            &[T("STAGED"), T("public.modifiedtable"), T("b"), T("added")],
                            &[T("WORKING"), T("public.modifiedtable"), T("b"), T("modified")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE table_name = 'public.droppedtable' ORDER BY commit_hash, table_name, column_name;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("STAGED"), T("public.droppedtable"), T("a"), T("added")],
                            &[T("STAGED"), T("public.droppedtable"), T("b"), T("added")],
                            &[T("WORKING"), T("public.droppedtable"), T("a"), T("removed")],
                            &[T("WORKING"), T("public.droppedtable"), T("b"), T("removed")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE table_name = 'public.renamedtable' OR table_name = 'public.newrenamedtable' ORDER BY commit_hash, table_name, column_name;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("STAGED"), T("public.renamedtable"), T("a"), T("added")],
                            &[T("STAGED"), T("public.renamedtable"), T("b"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE table_name = 'public.addedtable' ORDER BY commit_hash, table_name, column_name;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("WORKING"), T("public.addedtable"), T("a"), T("added")],
                            &[T("WORKING"), T("public.addedtable"), T("b"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "add column - commit history",
            intent: "dolt_column_diff shows an added column in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t ADD COLUMN d int;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', dolt_commit('-m', 'updating d in t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_column_diff WHERE commit_hash = current_setting('test.Commit1');",
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
                    query: "SELECT table_name, column_name, diff_type FROM dolt_column_diff WHERE commit_hash = current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("d"), T("added")],
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
            name: "add column - working set",
            intent: "dolt_column_diff shows an added column as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t ADD COLUMN d int;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_column_diff WHERE commit_hash = 'STAGED';",
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
                    query: "SELECT table_name, column_name, diff_type FROM dolt_column_diff WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("d"), T("added")],
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
            name: "modify column - commit history",
            intent: "dolt_column_diff shows a column whose values changed in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "UPDATE t SET c = 5 WHERE pk = 3;",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'updating value in t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_column_diff WHERE commit_hash = current_setting('test.Commit1');",
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
                    query: "SELECT table_name, column_name, diff_type FROM dolt_column_diff WHERE commit_hash = current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c"), T("modified")],
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
            name: "modify column - working set",
            intent: "dolt_column_diff shows a column whose values changed as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT dolt_add('.');",
                "UPDATE t SET c = 5 WHERE pk = 3;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_column_diff WHERE commit_hash = 'STAGED';",
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
                    query: "SELECT table_name, column_name, diff_type FROM dolt_column_diff WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c"), T("modified")],
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
            name: "drop column - commit history",
            intent: "dolt_column_diff shows a dropped column in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c in t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_column_diff WHERE commit_hash = current_setting('test.Commit1');",
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
                    query: "SELECT table_name, column_name, diff_type FROM dolt_column_diff WHERE commit_hash = current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c"), T("removed")],
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
            name: "drop column - working set",
            intent: "dolt_column_diff shows a dropped column as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t DROP COLUMN c;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_column_diff WHERE commit_hash = 'STAGED';",
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
                    query: "SELECT table_name, column_name, diff_type FROM dolt_column_diff WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c"), T("removed")],
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
            name: "drop column and recreate with same type - commit history",
            intent: "dolt_column_diff shows a column dropped and added again in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop column and recreate with same type - working set",
            intent: "dolt_column_diff shows a column dropped and added again as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (100, 101);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop column, then rename column with same type to same name - commit history",
            intent: "dolt_column_diff shows a column dropped and another renamed to its name in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c1'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c2 TO c1;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c1"), T("added")],
                            &[T("public.t"), T("c2"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c1"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c1"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop column, then rename column with same type to same name - working set",
            intent: "dolt_column_diff shows a column dropped and another renamed to its name as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t DROP COLUMN c1;",
                "ALTER TABLE t RENAME COLUMN c2 TO c1;",
                "INSERT INTO t VALUES (100, 101);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c1"), T("added")],
                            &[T("public.t"), T("c2"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c1"), T("removed")],
                            &[T("public.t"), T("c1"), T("modified")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "column drop and recreate with different type that can be coerced (int -> string) - commit history",
            intent: "dolt_column_diff shows a column added again with a type its values convert to in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c varchar(20);",
                "INSERT INTO t VALUES (100, '101');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 're-adding column c'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "column drop and recreate with different type that can be coerced (int -> string) - working set",
            intent: "dolt_column_diff shows a column added again with a type its values convert to as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c varchar(20);",
                "INSERT INTO t VALUES (100, '101');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c"), T("removed")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "column drop and recreate with different type that can NOT be coerced (string -> int) - commit history",
            intent: "dolt_column_diff shows a column added again with a type its values don't convert to in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 'two'), (3, 'four');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 're-adding column c'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "column drop and recreate with different type that can NOT be coerced (string -> int) - working set",
            intent: "dolt_column_diff shows a column added again with a type its values don't convert to as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c varchar(20));",
                "INSERT INTO t VALUES (1, 'two'), (3, 'four');",
                "SELECT dolt_add('.');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (100, 101);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c"), T("removed")],
                            &[T("public.t"), T("c"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple column renames - commit history",
            intent: "dolt_column_diff shows a column renamed, dropped, and added again in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c1 TO c2;",
                "INSERT INTO t VALUES (3, 4);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'renaming c1 to c2'), FALSE);",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'dropping column c2'), FALSE);",
                "ALTER TABLE t ADD COLUMN c2 int;",
                "INSERT INTO t VALUES (100, '101');",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'recreating column c2'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c1"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c2"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("c2"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit4');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c2"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple column renames - working set",
            intent: "dolt_column_diff shows a column renamed, dropped, and added again as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "INSERT INTO t VALUES (1, 2);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t RENAME COLUMN c1 TO c2;",
                "INSERT INTO t VALUES (3, 4);",
                "ALTER TABLE t DROP COLUMN c2;",
                "ALTER TABLE t ADD COLUMN c2 int;",
                "INSERT INTO t VALUES (100, '101');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c1"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c1"), T("removed")],
                            &[T("public.t"), T("c2"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary key change - commit history",
            intent: "dolt_column_diff shows a primary key change in the commit that changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "INSERT INTO t VALUES (5, 6);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping primary key'), FALSE);",
                "ALTER TABLE t ADD PRIMARY KEY (c1);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'adding primary key'), FALSE);",
                "INSERT INTO t VALUES (7, 8);",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'adding more data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_column_diff marks the columns whose values changed, and rows paired across a primary key change by a key both versions hold keep theirs, where Dolt diffs every row as removed and added, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c1"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c1"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and dolt_column_diff marks the columns whose values changed, and rows paired across a primary key change by a key both versions hold keep theirs, where Dolt diffs every row as removed and added, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash=current_setting('test.Commit4');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c1"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary key change - working set",
            intent: "dolt_column_diff shows a primary key change as a WORKING or STAGED change.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "ALTER TABLE t ADD PRIMARY KEY (c1);",
                "INSERT INTO t VALUES (7, 8);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_COLUMN_DIFF;",
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
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("added")],
                            &[T("public.t"), T("c1"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name, diff_type FROM DOLT_COLUMN_DIFF WHERE commit_hash='WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.t"), T("pk"), T("modified")],
                            &[T("public.t"), T("c1"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json column change",
            intent: "dolt_column_diff shows a json column whose value changed.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{\"test\": 123}');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit1');",
                "UPDATE t SET j = '{\"nottest\": 321}';",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, diff_type FROM dolt_column_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("j"), T("modified")],
                            &[T("pk"), T("added")],
                            &[T("j"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_column_diff committer columns when unset reflect --author",
            intent: "A commit made with --author shows the author as dolt_column_diff's committer too.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c VARCHAR(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table', '--author', 'Test Author <author@example.com>');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT committer, email, author, author_email FROM dolt_column_diff WHERE message = 'create table' LIMIT 1;",
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
    ]);
}

#[test]
fn test_commit_diff_system_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "error handling",
            intent: "dolt_commit_diff_<t> refuses a query that doesn't name exactly one to_commit and one from_commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_COMMIT_DIFF_t;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error querying table dolt_commit_diff_t: dolt_commit_diff_* tables must be filtered to a single 'dolt_to_commit'", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_COMMIT_DIFF_t WHERE dolt_to_commit=current_setting('test.Commit1');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_COMMIT_DIFF_t WHERE dolt_from_commit=current_setting('test.Commit1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error querying table dolt_commit_diff_t: dolt_commit_diff_* tables must be filtered to a single 'dolt_to_commit'", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "base case: insert, update, delete",
            intent: "dolt_commit_diff_<t> shows the rows that differ between any two commits, with filters on its key columns.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit0', dolt_commit('-am', 'creating table t'), FALSE);",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'populating table t'), FALSE);",
                "UPDATE t SET c2=0 WHERE pk=1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'modifying row'), FALSE);",
                "UPDATE t SET c2=-1 WHERE pk=1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'modifying row'), FALSE);",
                "UPDATE t SET c2=-2 WHERE pk=1;",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'modifying row'), FALSE);",
                "DELETE FROM t WHERE pk=1;",
                "SELECT set_config('test.Commit5', ('')::text, FALSE);",
                "SELECT set_config('test.Commit5', dolt_commit('-am', 'modifying row'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') AND dolt_FROM_COMMIT=current_setting('test.Commit0');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_T WHERE dolt_TO_COMMIT=current_setting('test.Commit1') AND dolt_FROM_COMMIT=current_setting('test.Commit0');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') AND dolt_FROM_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("0"), T("1"), T("2"), T("3"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_T WHERE dolt_TO_COMMIT=current_setting('test.Commit4') AND dolt_FROM_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("-2"), T("1"), T("2"), T("3"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_commit_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit5') AND dolt_FROM_COMMIT=current_setting('test.Commit4') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("2"), T("-2"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit5') AND dolt_FROM_COMMIT=current_setting('test.Commit0') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit5') AND dolt_FROM_COMMIT=current_setting('test.Commit0') AND from_pk IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit5') AND dolt_FROM_COMMIT=current_setting('test.Commit3') AND from_pk IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("2"), T("-1"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_commit_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit5') AND dolt_FROM_COMMIT=current_setting('test.Commit4') AND to_pk IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("2"), T("-2"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_commit_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit5') AND dolt_FROM_COMMIT=current_setting('test.Commit0') AND to_pk IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_commit_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') AND dolt_FROM_COMMIT=current_setting('test.Commit0') AND to_pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_commit_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit0') AND dolt_FROM_COMMIT=current_setting('test.Commit1') AND from_pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("2"), T("3"), T("removed")],
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
            name: "schema modification: column drop",
            intent: "dolt_commit_diff_<t> leaves out a dropped column and shows its rows as modified.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c2, from_pk, from_c2 FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') AND dolt_FROM_COMMIT=current_setting('test.Commit0') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c2", INT4)],
                        rows: &[
                            &[T("1"), T("3"), Null, Null],
                            &[T("4"), T("6"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c2, from_pk, from_c2 FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') AND dolt_FROM_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c2", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("1"), T("3")],
                            &[T("4"), T("6"), T("4"), T("6")],
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
            name: "schema modification: column drop, recreate with same type",
            intent: "A column dropped and added again reads its old values by name in dolt_commit_diff_<t>.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') AND dolt_FROM_COMMIT=current_setting('test.Commit0') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null, T("added")],
                            &[T("3"), T("4"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') AND dolt_FROM_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("2"), T("modified")],
                            &[T("3"), T("3"), T("4"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') AND dolt_FROM_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
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
            name: "schema modification: column drop, rename column with same type to same name",
            intent: "A column renamed to a dropped column's name reads the dropped column's values in dolt_commit_diff_<t>.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c1'), FALSE);",
                "ALTER TABLE t RENAME COLUMN c2 TO c1;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'inserting into t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') AND dolt_FROM_COMMIT=current_setting('test.Commit0') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("3"), Null, Null, T("added")],
                            &[T("4"), T("6"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and Dolt pairs columns other than the key by name, so a renamed column's values from before the rename show as NULL, where columns pair by column id first, Appendix C \"a column renamed between two versions of a table\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') AND dolt_FROM_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("3"), T("1"), T("3"), T("modified")],
                            &[T("4"), T("6"), T("4"), T("6"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and Dolt pairs columns other than the key by name, so a renamed column's values from before the rename show as NULL, where columns pair by column id first, Appendix C \"a column renamed between two versions of a table\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') AND dolt_FROM_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
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
            name: "working, staged, and head commits",
            intent: "dolt_commit_diff_<t> compares WORKING, STAGED, HEAD, and ancestry specs, and reads AS OF a branch.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_commit('-Am', 'created table');",
                "SELECT dolt_branch('Commit0');",
                "INSERT INTO t VALUES (7, 8, 9);",
                "SELECT dolt_commit('-Am', 'insert into table');",
                "SELECT dolt_branch('Commit1');",
                "INSERT INTO t VALUES (1, 2, 3);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (4, 5, 6);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT='WORKING' AND dolt_FROM_COMMIT='HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT='WORKING' AND dolt_FROM_COMMIT='HEAD~';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                            &[T("7"), T("8"), T("9"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT='STAGED' AND dolt_FROM_COMMIT='HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT='WORKING' AND dolt_FROM_COMMIT='STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("5"), T("6"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT='STAGED' AND dolt_FROM_COMMIT='WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("4"), T("5"), T("6"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM DOLT_COMMIT_DIFF_t AS OF Commit1 WHERE dolt_TO_COMMIT='HEAD' AND dolt_FROM_COMMIT='Commit0';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("7"), T("8"), T("9"), Null, Null, Null, T("added")],
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
            name: "detached head",
            intent: "dolt_commit_diff_<t> works in a revision database at a tag, where HEAD is detached.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'insert 1');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_tag('v1', current_setting('test.commit2'));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "use \"postgres/v1\";",
                    expected: Expected::Ok,
                    skip: Some("the script switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's client databases"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) AS table_diff_num FROM dolt_commit_diff_t WHERE dolt_from_commit=current_setting('test.commit1') AND dolt_to_commit=current_setting('test.commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("table_diff_num", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) AS table_diff_num FROM dolt_commit_diff_t WHERE dolt_from_commit=current_setting('test.commit1') AND dolt_to_commit='STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("table_diff_num", INT8)],
                        rows: &[
                            &[T("1")],
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
            name: "schema modification: column drop, recreate with different type that can be coerced (int -> string)",
            intent: "A column added again with a type its old values convert to reads them converted in dolt_commit_diff_<t>.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c varchar(20);",
                "INSERT INTO t VALUES (100, '101');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 're-adding column c'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') AND dolt_FROM_COMMIT=current_setting('test.Commit0') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", VARCHAR), Column("from_pk", INT4), Column("from_c", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null, T("added")],
                            &[T("3"), T("4"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') AND dolt_FROM_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", VARCHAR), Column("from_pk", INT4), Column("from_c", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("1"), T("2"), T("modified")],
                            &[T("3"), Null, T("3"), T("4"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') AND dolt_FROM_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", VARCHAR), Column("from_pk", INT4), Column("from_c", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
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
            name: "schema modification: column drop, recreate with different type that can't be coerced (string -> int)",
            intent: "Old values that don't convert to a column's new type read as NULL in dolt_commit_diff_<t>, with a warning for each.",
            set_up_script: &[
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 'two'), (3, 'four');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping column c'), FALSE);",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (100, 101);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 're-adding column c'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit1') AND dolt_FROM_COMMIT=current_setting('test.Commit0') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, Null, Null, T("added")],
                            &[T("3"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit2') AND dolt_FROM_COMMIT=current_setting('test.Commit1') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("1"), Null, T("modified")],
                            &[T("3"), Null, T("3"), Null, T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c, from_pk, from_c, dolt_diff_type FROM DOLT_COMMIT_DIFF_t WHERE dolt_TO_COMMIT=current_setting('test.Commit3') AND dolt_FROM_COMMIT=current_setting('test.Commit2') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c", INT4), Column("from_pk", INT4), Column("from_c", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("101"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_diff_t WHERE dolt_to_commit=current_setting('test.Commit3') AND dolt_from_commit=current_setting('test.Commit1');",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'c' into latest column schema", ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema modification: primary key change",
            intent: "dolt_commit_diff_<t> across a primary key change warns and shows nothing, while commits after it diff normally.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "INSERT INTO t VALUES (5, 6);",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping primary key'), FALSE);",
                "ALTER TABLE t ADD PRIMARY KEY (c1);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'adding primary key'), FALSE);",
                "INSERT INTO t VALUES (7, 8);",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-am', 'adding more data'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_diff_t WHERE dolt_from_commit=current_setting('test.Commit1') AND dolt_to_commit=current_setting('test.Commit4');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_commit_DIFF_t WHERE dolt_from_commit=current_setting('test.Commit3') AND dolt_to_commit=current_setting('test.Commit4');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("7"), T("8"), Null, Null, T("added")],
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
            name: "added and dropped table",
            intent: "dolt_commit_diff_<t> warns and shows nothing between commits where the table doesn't exist on a side.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2), (3, 4);",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "DROP TABLE t;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'dropping table'), FALSE);",
                "CREATE TABLE unrelated (a int PRIMARY KEY);",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-Am', 'created unrelated table'), FALSE);",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 2);",
                "SELECT set_config('test.Commit4', ('')::text, FALSE);",
                "SELECT set_config('test.Commit4', dolt_commit('-Am', 'recreating table t'), FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_diff_t WHERE dolt_from_commit=current_setting('test.Commit2') AND dolt_to_commit=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "due to primary key set change", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_diff_t WHERE dolt_from_commit=current_setting('test.Commit3') AND dolt_to_commit=current_setting('test.Commit3');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "due to primary key set change", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_commit_DIFF_t WHERE dolt_from_commit=current_setting('test.Commit3') AND dolt_to_commit=current_setting('test.Commit4');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM DOLT_commit_DIFF_t WHERE dolt_from_commit=current_setting('test.Commit1') AND dolt_to_commit=current_setting('test.Commit4');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("3"), T("4"), T("removed")],
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
            name: "Left outer join on dolt_commit_diff",
            intent: "dolt_commit_diff_<t> works as the inner side of an outer join.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'Create table t on main');",
                "INSERT INTO t VALUES (1, 'one'), (2, 'two');",
                "SELECT dolt_commit('-Am', 'Adding two rows to t on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t LEFT JOIN (SELECT from_pk, from_c1 FROM dolt_commit_diff_t WHERE dolt_from_commit = dolt_hashof('HEAD~') AND dolt_to_commit = dolt_hashof('HEAD')) AS dt ON 1 = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one"), Null, Null],
                            &[T("1"), T("one"), Null, Null],
                            &[T("2"), T("two"), Null, Null],
                            &[T("2"), T("two"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_schema_diff_table_function_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "basic schema changes",
            intent: "dolt_schema_diff shows each table created, dropped, renamed, or altered between two revisions, with both sides' create statements.",
            set_up_script: &[
                "CREATE TABLE employees (pk int PRIMARY KEY, name varchar(50));",
                "CREATE TABLE vacations (pk int PRIMARY KEY, name varchar(50));",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit0', ('')::text, FALSE);",
                "SELECT set_config('test.Commit0', dolt_commit('-am', 'commit 0'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "CREATE TABLE inventory (pk int PRIMARY KEY, name varchar(50), quantity int);",
                "DROP TABLE employees;",
                "ALTER TABLE vacations RENAME TO trips;",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'commit 1'), FALSE);",
                "SELECT dolt_tag('tag1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "ALTER TABLE inventory DROP COLUMN quantity, ADD COLUMN color varchar(10);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-m', 'commit 2'), FALSE);",
                "SELECT dolt_tag('tag2');",
                "SELECT * FROM dolt_checkout('-b', 'branch3');",
                "INSERT INTO inventory VALUES (1, 2, 3);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-m', 'commit 3'), FALSE);",
                "SELECT dolt_tag('tag3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_schema_diff() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_schema_diff: There are less than 2 arguments present, and the first does not contain '..'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_schema_diff: There are less than 2 arguments present, and the first does not contain '..'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branc1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_schema_diff: There are less than 2 arguments present, and the first does not contain '..'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_schema_diff: There are less than 2 arguments present, and the first does not contain '..'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "expected strings for from and to revisions, got: HEAD, ", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag1', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "expected strings for from and to revisions, got: tag1, ", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'inventory');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: inventory", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('inventory');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_schema_diff: There are less than 2 arguments present, and the first does not contain '..'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag3', 'tag4');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: tag4", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag3', 'tag4', 'inventory');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: tag4", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch1', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag1', 'tag1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 'fake-table');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag1', 'tag2', 'fake-table');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch1', 'branch2', 'fake-table');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit2'), current_setting('test.Commit3'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit2'), current_setting('test.Commit3'), 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag2', 'tag3');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag2', 'tag3', 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch2', 'branch3');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch2', 'branch3', 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit0'), current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.employees"), T(""), T("CREATE TABLE public.employees (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.employees OWNER TO postgres;\n\nALTER TABLE ONLY public.employees\n    ADD CONSTRAINT employees_pkey PRIMARY KEY (pk);"), T("")],
                            &[T("public.vacations"), T("public.trips"), T("CREATE TABLE public.vacations (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.vacations OWNER TO postgres;\n\nALTER TABLE ONLY public.vacations\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.trips (\n    pk integer CONSTRAINT vacations_pk_not_null NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.trips OWNER TO postgres;\n\nALTER TABLE ONLY public.trips\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);")],
                            &[T(""), T("public.inventory"), T(""), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit0'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T(""), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("")],
                            &[T("public.trips"), T("public.vacations"), T("CREATE TABLE public.trips (\n    pk integer CONSTRAINT vacations_pk_not_null NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.trips OWNER TO postgres;\n\nALTER TABLE ONLY public.trips\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.vacations (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.vacations OWNER TO postgres;\n\nALTER TABLE ONLY public.vacations\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);")],
                            &[T(""), T("public.employees"), T(""), T("CREATE TABLE public.employees (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.employees OWNER TO postgres;\n\nALTER TABLE ONLY public.employees\n    ADD CONSTRAINT employees_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit0'), current_setting('test.Commit1'), 'employees');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.employees"), T(""), T("CREATE TABLE public.employees (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.employees OWNER TO postgres;\n\nALTER TABLE ONLY public.employees\n    ADD CONSTRAINT employees_pkey PRIMARY KEY (pk);"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit0'), 'employees');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("public.employees"), T(""), T("CREATE TABLE public.employees (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.employees OWNER TO postgres;\n\nALTER TABLE ONLY public.employees\n    ADD CONSTRAINT employees_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit0'), current_setting('test.Commit1'), 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("public.inventory"), T(""), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit0'), 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T(""), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit0'), current_setting('test.Commit1'), 'trips');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.vacations"), T("public.trips"), T("CREATE TABLE public.vacations (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.vacations OWNER TO postgres;\n\nALTER TABLE ONLY public.vacations\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.trips (\n    pk integer CONSTRAINT vacations_pk_not_null NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.trips OWNER TO postgres;\n\nALTER TABLE ONLY public.trips\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit0'), 'trips');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.trips"), T("public.vacations"), T("CREATE TABLE public.trips (\n    pk integer CONSTRAINT vacations_pk_not_null NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.trips OWNER TO postgres;\n\nALTER TABLE ONLY public.trips\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.vacations (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.vacations OWNER TO postgres;\n\nALTER TABLE ONLY public.vacations\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit0'), current_setting('test.Commit1'), 'vacations');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.vacations"), T("public.trips"), T("CREATE TABLE public.vacations (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.vacations OWNER TO postgres;\n\nALTER TABLE ONLY public.vacations\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.trips (\n    pk integer CONSTRAINT vacations_pk_not_null NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.trips OWNER TO postgres;\n\nALTER TABLE ONLY public.trips\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit0'), 'vacations');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.trips"), T("public.vacations"), T("CREATE TABLE public.trips (\n    pk integer CONSTRAINT vacations_pk_not_null NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.trips OWNER TO postgres;\n\nALTER TABLE ONLY public.trips\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.vacations (\n    pk integer NOT NULL,\n    name character varying(50)\n);\n\nALTER TABLE public.vacations OWNER TO postgres;\n\nALTER TABLE ONLY public.vacations\n    ADD CONSTRAINT vacations_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit1'), current_setting('test.Commit2'), 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch1..branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch1...branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch1', 'branch2', 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag1', 'tag2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag1..tag2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag1...tag2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag1', 'tag2', 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit2'), current_setting('test.Commit1'));",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff(current_setting('test.Commit2'), current_setting('test.Commit1'), 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch2', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('branch2', 'branch1', 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag2', 'tag1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('tag2', 'tag1', 'inventory');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.inventory"), T("public.inventory"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    color character varying(10)\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.inventory (\n    pk integer NOT NULL,\n    name character varying(50),\n    quantity integer\n);\n\nALTER TABLE public.inventory OWNER TO postgres;\n\nALTER TABLE ONLY public.inventory\n    ADD CONSTRAINT inventory_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "prepared table functions",
            intent: "The diff functions take a prepared statement's parameters as their arguments.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY);",
                "INSERT INTO t1 VALUES (0), (1);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit0', ('')::text, FALSE);",
                "SELECT set_config('test.Commit0', dolt_commit('-am', 'commit 0'), FALSE);",
                "ALTER TABLE t1 ADD COLUMN b int DEFAULT 1;",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'commit 1'), FALSE);",
                "CREATE TABLE t2 (a int PRIMARY KEY);",
                "INSERT INTO t2 VALUES (0), (1);",
                "INSERT INTO t1 VALUES (2,2), (3,2);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'commit 2'), FALSE);",
                "PREPARE sch_diff AS SELECT count(*) FROM dolt_schema_diff($1,$2,$3);",
                "PREPARE diff_stat AS SELECT count(*) FROM dolt_diff_stat($1,$2,$3);",
                "PREPARE diff_sum AS SELECT count(*) FROM dolt_diff_summary($1,$2,$3);",
                "PREPARE patch AS SELECT count(*) FROM dolt_schema_diff($1,$2,$3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.t1_name', ('t1')::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE sch_diff(current_setting('test.Commit0'), current_setting('test.Commit1'), current_setting('test.t1_name'));",
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
                    query: "EXECUTE diff_stat(current_setting('test.Commit1'), current_setting('test.Commit2'), current_setting('test.t1_name'));",
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
                    query: "EXECUTE diff_sum(current_setting('test.Commit1'), current_setting('test.Commit2'), current_setting('test.t1_name'));",
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
                    query: "EXECUTE patch(current_setting('test.Commit0'), current_setting('test.Commit1'), current_setting('test.t1_name'));",
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
fn test_dolt_database_collation_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "can't use __DATABASE__ prefix in table names",
            intent: "Dolt reserves table names beginning with __DATABASE__ for database-level data.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE __DATABASE__t(i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Doltgrust keeps database-level data apart from tables, so no table name is reserved, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "db collation change with dolt_add('.')",
            intent: "A database collation change is staged and committed like a table.",
            assertions: &[
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
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER database postgres ;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.__database__mydb"), Any, T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("WORKING"), T("public.__database__mydb"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.__database__mydb"), Any, T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("STAGED"), T("public.__database__mydb"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("checkpoint enginetest database postgres")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'db collation changed');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.__database__mydb"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("db collation changed")],
                            &[T("checkpoint enginetest database postgres")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a Postgres database's collation is fixed when it is made"),
            ..S
        },
        ScriptTest {
            name: "db collation change with dolt_add('__DATABASE__mydb')",
            intent: "A database collation change is staged by its pseudo-table name.",
            assertions: &[
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
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER database postgres ;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.__database__mydb"), Any, T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("WORKING"), T("public.__database__mydb"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('__DATABASE__mydb');",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.__database__mydb"), Any, T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("STAGED"), T("public.__database__mydb"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("checkpoint enginetest database postgres")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'db collation changed');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.__database__mydb"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("db collation changed")],
                            &[T("checkpoint enginetest database postgres")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a Postgres database's collation is fixed when it is made"),
            ..S
        },
        ScriptTest {
            name: "db collation change with dolt_commit('-Am', '')",
            intent: "dolt_commit -A commits a database collation change.",
            assertions: &[
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
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER database postgres ;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.__database__mydb"), Any, T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("WORKING"), T("public.__database__mydb"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("checkpoint enginetest database postgres")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'db collation changed');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT table_name, data_change, schema_change FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.__database__mydb"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("db collation changed")],
                            &[T("checkpoint enginetest database postgres")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a Postgres database's collation is fixed when it is made"),
            ..S
        },
        ScriptTest {
            name: "db collation hard reset",
            intent: "dolt_reset --hard undoes a database collation change.",
            set_up_script: &[
                "ALTER database postgres ;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.__database__mydb"), Any, T("modified")],
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
            skip: Some("MySQL-only: a Postgres database's collation is fixed when it is made"),
            ..S
        },
        ScriptTest {
            name: "db collation with branch",
            intent: "A database collation change belongs to the branch that made it.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER database postgres ;",
                "SELECT dolt_commit('-Am', 'db collation');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "show CREATE database postgres;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("postgres"), T("CREATE DATABASE `postgres` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_spanish_ci */")],
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
                    query: "show CREATE database postgres;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("postgres"), T("CREATE DATABASE `postgres` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin */")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a Postgres database's collation is fixed when it is made"),
            ..S
        },
        ScriptTest {
            name: "db collation with ff merge",
            intent: "A fast-forward merge brings a database collation change.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER database postgres ;",
                "SELECT dolt_commit('-Am', 'db collation');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "show CREATE database postgres;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("postgres"), T("CREATE DATABASE `postgres` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin */")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "show CREATE database postgres;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("postgres"), T("CREATE DATABASE `postgres` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_spanish_ci */")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a Postgres database's collation is fixed when it is made"),
            ..S
        },
        ScriptTest {
            name: "db collation merge conflict",
            intent: "Different database collation changes on two branches conflict at merge.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "ALTER database postgres ;",
                "SELECT dolt_commit('-Am', 'main collation');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER database postgres ;",
                "SELECT dolt_commit('-Am', 'main collation');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "database collation conflict, please resolve manually. ours: utf8mb4_danish_ci, theirs: utf8mb4_spanish_ci", ..E }),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a Postgres database's collation is fixed when it is made"),
            ..S
        },
    ]);
}

#[test]
fn test_query_diff_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "basic query diff tests",
            intent: "dolt_query_diff compares two queries' rows by key, refusing queries that aren't valid SELECT statements.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE TABLE tt (i int PRIMARY KEY, j int);",
                "INSERT INTO tt VALUES (10, 10), (20, 20), (30, 30);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'first');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET j = 10 WHERE i = 2;",
                "DELETE FROM t WHERE i = 3;",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_query_diff() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('selectsyntaxerror', 'selectsyntaxerror');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "selectsyntaxerror""#, ..E }),
                    dolt: Some(Dolt::Differs("the queries are parsed with Postgres' grammar, so a syntax error is Postgres', Appendix C \"arguments of the diff functions\"")),
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "query must be a SELECT statement", ..E }),
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('create table tt (i int)', 'create table ttt (j int)');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "query must be a SELECT statement", ..E }),
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from missingtable', '');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "missingtable" does not exist"#, ..E }),
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from t as of other', 'select * from t as of head');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("2"), T("2"), T("10"), T("modified")],
                            &[T("3"), T("3"), Null, Null, T("deleted")],
                            &[Null, Null, T("4"), T("4"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from t as of head', 'select * from t as of other');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("10"), T("2"), T("2"), T("modified")],
                            &[Null, Null, T("3"), T("3"), T("added")],
                            &[T("4"), T("4"), Null, Null, T("deleted")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from t as of other where i = 2', 'select * from t as of head where i = 2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("2"), T("2"), T("10"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from t as of other where i < 2', 'select * from t as of head where i > 2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null, T("deleted")],
                            &[Null, Null, T("4"), T("4"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from t', 'select * from tt');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null, T("deleted")],
                            &[T("2"), T("10"), Null, Null, T("deleted")],
                            &[T("4"), T("4"), Null, Null, T("deleted")],
                            &[Null, Null, T("10"), T("10"), T("added")],
                            &[Null, Null, T("20"), T("20"), T("added")],
                            &[Null, Null, T("30"), T("30"), T("added")],
                        ],
                        tag: "SELECT 6",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff_summary respects dolt_ignore: basic ignore functionality",
            intent: "dolt_diff_summary leaves out new tables dolt_ignore names.",
            set_up_script: &[
                "CREATE TABLE ignored_table (pk int PRIMARY KEY, c1 int);",
                "CREATE TABLE not_ignored_table (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'add tables');",
                "INSERT INTO dolt_ignore VALUES ('ignored_table2', TRUE);",
                "CREATE TABLE ignored_table2 (pk int PRIMARY KEY, c2 int);",
                "CREATE TABLE not_ignored_table2 (pk int PRIMARY KEY, c2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING');",
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
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING') ORDER BY to_table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("dolt_ignore")],
                            &[T("public.not_ignored_table2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff_summary respects dolt_ignore: wildcard patterns",
            intent: "dolt_diff_summary leaves out new tables a dolt_ignore pattern matches.",
            set_up_script: &[
                "CREATE TABLE initial_table (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial');",
                "INSERT INTO dolt_ignore VALUES ('temp_*', TRUE);",
                "CREATE TABLE temp_table1 (pk int PRIMARY KEY);",
                "CREATE TABLE temp_table2 (pk int PRIMARY KEY);",
                "CREATE TABLE regular_table (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING');",
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
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING') ORDER BY to_table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("dolt_ignore")],
                            &[T("public.regular_table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff_summary respects dolt_ignore: mixed ignore/don't ignore patterns",
            intent: "A dolt_ignore pattern that doesn't ignore wins over a wider one that does, in dolt_diff_summary.",
            set_up_script: &[
                "CREATE TABLE initial_table (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial');",
                "INSERT INTO dolt_ignore VALUES ('temp_*', TRUE);",
                "INSERT INTO dolt_ignore VALUES ('temp_important', FALSE);",
                "CREATE TABLE temp_table1 (pk int PRIMARY KEY);",
                "CREATE TABLE temp_important (pk int PRIMARY KEY);",
                "CREATE TABLE regular_table (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING');",
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
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING') ORDER BY to_table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("dolt_ignore")],
                            &[T("public.regular_table")],
                            &[T("public.temp_important")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff_summary respects dolt_ignore: specific table query",
            intent: "dolt_diff_summary of an ignored new table is empty.",
            set_up_script: &[
                "CREATE TABLE initial_table (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial');",
                "INSERT INTO dolt_ignore VALUES ('ignored_table', TRUE);",
                "CREATE TABLE ignored_table (pk int PRIMARY KEY);",
                "CREATE TABLE not_ignored_table (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING', 'ignored_table');",
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
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING', 'not_ignored_table');",
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
            name: "dolt_diff_summary respects dolt_ignore: dropped tables",
            intent: "dolt_diff_summary still shows a dropped table that dolt_ignore names.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('dummy', TRUE);",
                "CREATE TABLE will_be_ignored (pk int PRIMARY KEY);",
                "CREATE TABLE will_not_be_ignored (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'add tables');",
                "DELETE FROM dolt_ignore WHERE pattern = 'dummy';",
                "INSERT INTO dolt_ignore VALUES ('will_be_ignored', TRUE);",
                "DROP TABLE will_be_ignored;",
                "DROP TABLE will_not_be_ignored;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING');",
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
                    query: "SELECT from_table_name, diff_type FROM dolt_diff_summary('HEAD', 'WORKING') ORDER BY from_table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("dolt_ignore"), T("modified")],
                            &[T("public.will_not_be_ignored"), T("dropped")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff_summary commit-to-commit shows tables added to dolt_ignore after the commit",
            intent: "Ignoring a table later doesn't hide it from dolt_diff_summary between earlier commits.",
            set_up_script: &[
                "CREATE TABLE the_table (pk int PRIMARY KEY, c1 int);",
                "INSERT INTO the_table VALUES (1, 10), (2, 20);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'add the_table');",
                "INSERT INTO dolt_ignore VALUES ('the_table', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'ignore the_table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_table_name, diff_type FROM dolt_diff_summary('HEAD~2', 'HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.the_table"), T("added")],
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
            name: "dolt_query_diff in read-only transaction",
            intent: "dolt_query_diff runs in a read-only transaction.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "CREATE TABLE tt (i int PRIMARY KEY, j int);",
                "INSERT INTO tt VALUES (1, 1), (2, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION READ ONLY;",
                    expected: Expected::Tag("START TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('SELECT * FROM t', 'SELECT * FROM tt');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null, T("deleted")],
                            &[T("2"), T("2"), Null, Null, T("deleted")],
                            &[Null, Null, T("1"), T("1"), T("added")],
                            &[Null, Null, T("2"), T("3"), T("added")],
                        ],
                        tag: "SELECT 4",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt1_diff_system_table_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "Diff table stops creating diff partitions when any primary key type has changed",
            intent: "dolt_diff_<table> stops at a commit that changed a primary key column's type, showing only the changes after it.",
            set_up_script: &[
                "CREATE TABLE t (pk1 VARCHAR(100), pk2 VARCHAR(100), PRIMARY KEY (pk1, pk2));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES ('1', '1');",
                "SELECT dolt_commit('-am', 'setup');",
                "ALTER TABLE t ALTER COLUMN pk2 TYPE VARCHAR(101), ALTER COLUMN pk2 SET NOT NULL, ALTER COLUMN pk2 DROP IDENTITY IF EXISTS, ALTER COLUMN pk2 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'modify column type');",
                "INSERT INTO t VALUES ('2', '2');",
                "SELECT dolt_commit('-am', 'insert new row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk1, to_pk2, from_pk1, from_pk2, dolt_diff_type FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk1", VARCHAR), Column("to_pk2", VARCHAR), Column("from_pk1", VARCHAR), Column("from_pk2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("2"), Null, Null, T("added")],
                            &[T("1"), T("1"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's dolt_diff_<t> stops at a commit that changed a primary key column's type, which its storage can't compare across; Doltgrust's diff continues through it. Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_system_table_index_tests() {
    run_scripts(&[
        ScriptTest {
            name: "systab benchmarks",
            intent: "Filters and joins on the commit columns of the version control system tables find the rows of one commit.",
            set_up_script: &[
                "CREATE TABLE xy (x int PRIMARY KEY, y varchar(20));",
                "INSERT INTO xy VALUES (0, 'row 0'), (1, 'row 1'), (2, 'row 2'), (3, 'row 3'), (4, 'row 4');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit 0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x < 10;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (20, 'row 20'), (21, 'row 21'), (22, 'row 22'), (23, 'row 23'), (24, 'row 24');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 10 AND x < 30;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (40, 'row 40'), (41, 'row 41'), (42, 'row 42'), (43, 'row 43'), (44, 'row 44');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 30 AND x < 50;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (60, 'row 60'), (61, 'row 61'), (62, 'row 62'), (63, 'row 63'), (64, 'row 64');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 50 AND x < 70;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (80, 'row 80'), (81, 'row 81'), (82, 'row 82'), (83, 'row 83'), (84, 'row 84');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 4');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.commit', ((SELECT commit_hash FROM dolt_log WHERE message = 'commit 2'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_x, to_x FROM dolt_diff_xy WHERE dolt_to_commit = current_setting('test.commit', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("from_x", INT4), Column("to_x", INT4)],
                        rows: &[
                            &[T("20"), T("20")],
                            &[T("21"), T("21")],
                            &[T("22"), T("22")],
                            &[T("23"), T("23")],
                            &[T("24"), T("24")],
                            &[Null, T("40")],
                            &[Null, T("41")],
                            &[Null, T("42")],
                            &[Null, T("43")],
                            &[Null, T("44")],
                        ],
                        tag: "SELECT 10",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_x, to_x FROM dolt_diff_xy WHERE dolt_from_commit = current_setting('test.commit', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("from_x", INT4), Column("to_x", INT4)],
                        rows: &[
                            &[T("40"), T("40")],
                            &[T("41"), T("41")],
                            &[T("42"), T("42")],
                            &[T("43"), T("43")],
                            &[T("44"), T("44")],
                            &[Null, T("60")],
                            &[Null, T("61")],
                            &[Null, T("62")],
                            &[Null, T("63")],
                            &[Null, T("64")],
                        ],
                        tag: "SELECT 10",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff WHERE commit_hash = current_setting('test.commit', TRUE);",
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
                    query: "SELECT count(*) FROM dolt_history_xy WHERE dolt_commit_hash = current_setting('test.commit', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE commit_hash = current_setting('test.commit', TRUE);",
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
                    query: "SELECT count(*) FROM dolt_commits WHERE commit_hash = current_setting('test.commit', TRUE);",
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
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = current_setting('test.commit', TRUE);",
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
                    query: "SELECT count(*) FROM dolt_diff_xy JOIN dolt_log ON commit_hash = dolt_to_commit;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("45")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff_xy JOIN dolt_log ON commit_hash = dolt_from_commit;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("45")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_blame_xy;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("25")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*)\n           FROM dolt_commits AS cm\n           JOIN dolt_commit_ancestors AS an\n           ON cm.commit_hash = an.parent_hash\n           ORDER BY cm.date, cm.message ASC;",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "column \"cm.date\" must appear in the GROUP BY clause or be used in an aggregate function", position: 162, ..E }),
                    dolt: Some(Dolt::Differs("MySQL orders a query of only aggregates by columns outside them, which Postgres refuses")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) /*+ JOIN_ORDER(a,b) */ FROM dolt_diff_xy a JOIN xy b ON x = to_x;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) /*+ JOIN_ORDER(b,a) */ FROM dolt_diff_xy a JOIN xy b ON x = to_x;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "required index lookup in join",
            intent: "A tag joined to dolt_commit_diff_<table> on its commit and that commit's parent finds the tag.",
            set_up_script: &[
                "CREATE TABLE xy (x int PRIMARY KEY, y varchar(20));",
                "INSERT INTO xy VALUES (0, 'row 0'), (1, 'row 1'), (2, 'row 2'), (3, 'row 3'), (4, 'row 4');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit 0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x < 10;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (20, 'row 20'), (21, 'row 21'), (22, 'row 22'), (23, 'row 23'), (24, 'row 24');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 10 AND x < 30;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (40, 'row 40'), (41, 'row 41'), (42, 'row 42'), (43, 'row 43'), (44, 'row 44');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 30 AND x < 50;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (60, 'row 60'), (61, 'row 61'), (62, 'row 62'), (63, 'row 63'), (64, 'row 64');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 50 AND x < 70;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (80, 'row 80'), (81, 'row 81'), (82, 'row 82'), (83, 'row 83'), (84, 'row 84');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 4');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.tag_head', (dolt_hashof('main^'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('t1', concat(current_setting('test.tag_head', TRUE), '^'));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ HASH_JOIN(t,cd) */ DISTINCT t.tag_name\nFROM dolt_tags t\nLEFT JOIN dolt_commit_diff_xy cd\n    ON cd.dolt_to_commit = t.tag_name AND\n       cd.dolt_from_commit = concat(t.tag_name, '^');",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("t1")],
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
            name: "commit indexing edge cases",
            intent: "Filters on the commit columns of the system tables handle WORKING, several commits, branch heads, and unknown commits.",
            set_up_script: &[
                "CREATE TABLE xy (x int PRIMARY KEY, y varchar(20));",
                "INSERT INTO xy VALUES (0, 'row 0'), (1, 'row 1'), (2, 'row 2'), (3, 'row 3'), (4, 'row 4');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit 0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x < 10;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (20, 'row 20'), (21, 'row 21'), (22, 'row 22'), (23, 'row 23'), (24, 'row 24');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 10 AND x < 30;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (40, 'row 40'), (41, 'row 41'), (42, 'row 42'), (43, 'row 43'), (44, 'row 44');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 30 AND x < 50;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (60, 'row 60'), (61, 'row 61'), (62, 'row 62'), (63, 'row 63'), (64, 'row 64');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 50 AND x < 70;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES (80, 'row 80'), (81, 'row 81'), (82, 'row 82'), (83, 'row 83'), (84, 'row 84');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 4');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'feat');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'feat commit 1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'feat commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE xy SET y = '1' WHERE x > 70 AND x < 90;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's varchar arithmetic reads 'row 0' + 1 as 1, with a warning, which Postgres refuses, so the update stores MySQL's 1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.commit', ((SELECT commit_hash FROM dolt_log WHERE message = 'commit 1'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.root_commit', ((SELECT commit_hash FROM dolt_log WHERE message = 'Initialize data repository'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.feat_head', (dolt_hashof('feat'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.feat_head1', (dolt_hashof('feat~'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_x, to_x FROM dolt_diff_xy WHERE dolt_to_commit = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("from_x", INT4), Column("to_x", INT4)],
                        rows: &[
                            &[T("80"), T("80")],
                            &[T("81"), T("81")],
                            &[T("82"), T("82")],
                            &[T("83"), T("83")],
                            &[T("84"), T("84")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_xy WHERE dolt_from_commit = current_setting('test.feat_head1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("to_x", INT4), Column("to_y", VARCHAR), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_x", INT4), Column("from_y", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_xy WHERE dolt_from_commit = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("to_x", INT4), Column("to_y", VARCHAR), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_x", INT4), Column("from_y", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff WHERE commit_hash = 'WORKING';",
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
                    query: "SELECT count(*) FROM dolt_history_xy WHERE dolt_commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = 'WORKING';",
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
                    query: "SELECT sum(to_x) FROM dolt_diff_xy WHERE dolt_to_commit IN (current_setting('test.commit', TRUE), 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("530")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(to_x) FROM dolt_diff_xy WHERE dolt_from_commit IN (current_setting('test.commit', TRUE), 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("320")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff WHERE commit_hash IN (current_setting('test.commit', TRUE), 'WORKING');",
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
                    query: "SELECT sum(x) FROM dolt_history_xy WHERE dolt_commit_hash IN (current_setting('test.commit', TRUE), 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("120")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash IN (current_setting('test.commit', TRUE), current_setting('test.root_commit', TRUE));",
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
                    query: "SELECT count(*) FROM dolt_log WHERE commit_hash IN (current_setting('test.commit', TRUE), current_setting('test.root_commit', TRUE));",
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
                    query: "SELECT count(*) FROM dolt_log WHERE commit_hash = current_setting('test.feat_head', TRUE);",
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
                    query: "SELECT count(*) FROM dolt_commits WHERE commit_hash = current_setting('test.feat_head', TRUE);",
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
                    query: "SELECT count(*) FROM dolt_commits WHERE commit_hash IN (current_setting('test.commit', TRUE), current_setting('test.root_commit', TRUE));",
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
                    query: "SELECT from_x, to_x FROM dolt_diff_xy WHERE dolt_to_commit = 'unknown';",
                    expected: Expected::Rows {
                        columns: &[Column("from_x", INT4), Column("to_x", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_xy WHERE dolt_from_commit = 'unknown';",
                    expected: Expected::Rows {
                        columns: &[Column("to_x", INT4), Column("to_y", VARCHAR), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_x", INT4), Column("from_y", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff WHERE commit_hash = 'unknown';",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("committer", TEXT), Column("email", TEXT), Column("date", TIMESTAMPTZ), Column("message", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL), Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMPTZ)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_history_xy WHERE dolt_commit_hash = 'unknown';",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", VARCHAR), Column("dolt_commit_hash", TEXT), Column("dolt_committer", TEXT), Column("dolt_commit_date", TIMESTAMPTZ)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_ancestors WHERE commit_hash = 'unknown';",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("parent_hash", TEXT), Column("parent_index", INT4)],
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
            name: "empty log table",
            intent: "dolt_log joined to dolt_commit_ancestors in a database with only its initial commits counts them.",
            set_up_script: &[
                "CREATE TABLE xy (x int PRIMARY KEY, y int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log AS dc JOIN dolt_commit_ancestors AS dca ON dc.commit_hash = dca.commit_hash;",
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
            name: "from_commit at multiple heights, choose highest",
            intent: "dolt_diff_<table> filtered by a from_commit that several commits descend from lists the diff of every commit it is a parent of: main 3 against its first parent and the merge o3 against its second.",
            set_up_script: &[
                "CREATE TABLE x (x int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.m1h1', dolt_commit('-am', 'main 1'), FALSE);",
                "INSERT INTO x VALUES (1),(2);",
                "SELECT set_config('test.m2h2', dolt_commit('-am', 'main 2'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "SELECT dolt_reset('--hard', current_setting('test.m1h1', TRUE));",
                "INSERT INTO x VALUES (3),(4);",
                "SELECT set_config('test.o1h2', dolt_commit('-am', 'other 1'), FALSE);",
                "INSERT INTO x VALUES (5),(6);",
                "SELECT set_config('test.o2h3', dolt_commit('-am', 'other 2'), FALSE);",
                "SELECT * FROM dolt_merge('main');",
                "SELECT set_config('test.o3h4', (dolt_hashof('head~'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO x VALUES (7),(8);",
                "SELECT set_config('test.m32h3', dolt_commit('-am', 'main 3'), FALSE);",
                "SELECT * FROM dolt_merge('other');",
                "SELECT set_config('test.m4h5', (dolt_hashof('head~'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff_x WHERE dolt_from_commit = current_setting('test.m2h2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Bug("Dolt's diff table pairs each commit, as from_commit, with only one child (processCommit overwrites cmHashToTblInfo[parent] for each child, and fromCommitLookupPartitions picks the highest), which its own TODO says the iterator can't handle, so main 3's change against main 2 appears nowhere and Dolt counts o3's diff against its second parent instead. Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "indexes work when table doesn't exist on one branch",
            intent: "dolt_diff() between branches where only one has the table finds no rows of a commit filter that matches nothing.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'mod');",
                "CREATE TABLE newTable(id int);",
                "SELECT dolt_add('newTable');",
                "SELECT dolt_commit('-am', 'added new table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff('main', 'mod', 'newTable') WHERE dolt_to_commit='def';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff('mod', 'main', 'newTable') WHERE dolt_to_commit='def';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
