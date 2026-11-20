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

//! Dolt's integration-tests/bats/sql-commit-diff.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Dolt, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_commit_diff() {
    run_scripts(&[
        ScriptTest {
            name: "sql-commit-diff: DOLT_COMMIT_DIFF with a range on to_ key",
            intent: "A range on dolt_commit_diff_<t>'s to_ key columns finds the rows whose new key is in it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, v int);",
                "CREATE TABLE test_two_pk (pk1 int, pk2 int, v int, PRIMARY KEY (pk1, pk2));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table');",
                "SELECT dolt_branch('create');",
                "INSERT INTO test VALUES (10, 10), (11, 11), (20, 20), (21, 21), (30, 30), (31, 31);",
                "INSERT INTO test_two_pk (pk1, pk2) VALUES (1, 10), (1, 11), (2, 20), (2, 21), (3, 30), (3, 31);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial values');",
                "SELECT dolt_branch('initial');",
                "INSERT INTO test VALUES (12, 12), (22, 22), (32, 32);",
                "INSERT INTO test_two_pk (pk1, pk2) VALUES (1, 12), (2, 22), (3, 32);",
                "DELETE FROM test WHERE pk = 1 OR pk = 20 OR pk = 30;",
                "DELETE FROM test_two_pk WHERE pk2 = 10 OR pk2 = 20 OR pk2 = 30;",
                "UPDATE test SET v = 0 WHERE pk = 11 OR pk = 21 OR pk = 31;",
                "UPDATE test_two_pk SET v = 1 WHERE pk2 = 11 OR pk2 = 21 OR pk2 = 31;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'update values');",
                "SELECT dolt_branch('update');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, to_pk, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_from_commit = dolt_hashof('initial') AND dolt_to_commit = dolt_hashof('update') AND to_pk > 12 AND to_pk < 30;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("to_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("21"), T("21"), T("modified")],
                            &[Null, T("22"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, to_pk, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_from_commit = dolt_hashof('create') AND dolt_to_commit = dolt_hashof('HEAD') AND to_pk > 0 AND to_pk < 5;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("to_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("describe plan shows go-mysql-server's index ranges; dolt_commit_diff_<t> is a view over a function scan that takes the key range pushed into it, so this checks the rows that range finds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk1, from_pk2, to_pk1, to_pk2, dolt_diff_type FROM dolt_commit_diff_test_two_pk WHERE dolt_from_commit = dolt_hashof('initial') AND dolt_to_commit = dolt_hashof('update') AND to_pk1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk1", INT4), Column("from_pk2", INT4), Column("to_pk1", INT4), Column("to_pk2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("21"), T("2"), T("21"), T("modified")],
                            &[Null, Null, T("2"), T("22"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk1, from_pk2, to_pk1, to_pk2, dolt_diff_type FROM dolt_commit_diff_test_two_pk WHERE dolt_from_commit = dolt_hashof('create') AND dolt_to_commit = dolt_hashof('HEAD') AND to_pk1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk1", INT4), Column("from_pk2", INT4), Column("to_pk1", INT4), Column("to_pk2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("2"), T("21"), T("added")],
                            &[Null, Null, T("2"), T("22"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("describe plan shows go-mysql-server's index ranges; dolt_commit_diff_<t> is a view over a function scan that takes the key range pushed into it, so this checks the rows that range finds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit-diff: DOLT_COMMIT_DIFF with a range on from_ key",
            intent: "A range on dolt_commit_diff_<t>'s from_ key columns finds the rows whose old key is in it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, v int);",
                "CREATE TABLE test_two_pk (pk1 int, pk2 int, v int, PRIMARY KEY (pk1, pk2));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table');",
                "SELECT dolt_branch('create');",
                "INSERT INTO test VALUES (10, 10), (11, 11), (20, 20), (21, 21), (30, 30), (31, 31);",
                "INSERT INTO test_two_pk (pk1, pk2) VALUES (1, 10), (1, 11), (2, 20), (2, 21), (3, 30), (3, 31);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial values');",
                "SELECT dolt_branch('initial');",
                "INSERT INTO test VALUES (12, 12), (22, 22), (32, 32);",
                "INSERT INTO test_two_pk (pk1, pk2) VALUES (1, 12), (2, 22), (3, 32);",
                "DELETE FROM test WHERE pk = 1 OR pk = 20 OR pk = 30;",
                "DELETE FROM test_two_pk WHERE pk2 = 10 OR pk2 = 20 OR pk2 = 30;",
                "UPDATE test SET v = 0 WHERE pk = 11 OR pk = 21 OR pk = 31;",
                "UPDATE test_two_pk SET v = 1 WHERE pk2 = 11 OR pk2 = 21 OR pk2 = 31;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'update values');",
                "SELECT dolt_branch('update');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, to_pk, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_from_commit = dolt_hashof('initial') AND dolt_to_commit = dolt_hashof('update') AND from_pk > 12 AND from_pk < 30;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("to_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("20"), Null, T("removed")],
                            &[T("21"), T("21"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, to_pk, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_from_commit = dolt_hashof('create') AND dolt_to_commit = dolt_hashof('HEAD') AND from_pk > 0 AND from_pk < 5;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("to_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("describe plan shows go-mysql-server's index ranges; dolt_commit_diff_<t> is a view over a function scan that takes the key range pushed into it, so this checks the rows that range finds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk1, from_pk2, to_pk1, to_pk2, dolt_diff_type FROM dolt_commit_diff_test_two_pk WHERE dolt_from_commit = dolt_hashof('initial') AND dolt_to_commit = dolt_hashof('update') AND from_pk1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk1", INT4), Column("from_pk2", INT4), Column("to_pk1", INT4), Column("to_pk2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("20"), Null, Null, T("removed")],
                            &[T("2"), T("21"), T("2"), T("21"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk1, from_pk2, to_pk1, to_pk2, dolt_diff_type FROM dolt_commit_diff_test_two_pk WHERE dolt_from_commit = dolt_hashof('create') AND dolt_to_commit = dolt_hashof('HEAD') AND from_pk1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk1", INT4), Column("from_pk2", INT4), Column("to_pk1", INT4), Column("to_pk2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("describe plan shows go-mysql-server's index ranges; dolt_commit_diff_<t> is a view over a function scan that takes the key range pushed into it, so this checks the rows that range finds")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
