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

//! Dolt's integration-tests/bats/system-tables.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_system_tables() {
    run_scripts(&[
        ScriptTest {
            name: "system-tables: query dolt_log system table",
            intent: "dolt_log lists the commits, and filters on their messages.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added test table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'Added test table';",
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
                    query: "SELECT count(*) FROM dolt_log WHERE message != 'Added test table' AND message = 'Added test table';",
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
            name: "system-tables: dolt_log system table includes commit_order column",
            intent: "dolt_log has a numeric commit_order, distinct for each commit of a line of history.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added test table');",
                "SELECT dolt_commit('--allow-empty', '-m', 'Empty commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'dolt_log' AND column_name = 'commit_order';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("commit_order")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_order > 0 FROM dolt_log WHERE message = 'Added test table';",
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
                    query: "SELECT count(DISTINCT commit_order) FROM dolt_log;",
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
            name: "system-tables: dolt_log() table function includes commit_order column",
            intent: "dolt_log() gives a commit the same commit_order as the dolt_log table.",
            set_up_script: &[
                "CREATE TABLE test2 (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test2');",
                "SELECT dolt_commit('-m', 'Added test2 table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT commit_order > 0 FROM dolt_log() WHERE message = 'Added test2 table';",
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
                    query: "SELECT (SELECT commit_order FROM dolt_log WHERE message = 'Added test2 table') = (SELECT commit_order FROM dolt_log() WHERE message = 'Added test2 table') AS orders_match;",
                    expected: Expected::Rows {
                        columns: &[Column("orders_match", BOOL)],
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
            name: "system-tables: commit_order reflects topological order for branches",
            intent: "Sibling commits share a commit_order, and a merge commit's is higher.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "SELECT dolt_commit('--allow-empty', '-m', 'feature commit');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT dolt_commit('--allow-empty', '-m', 'main commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT (SELECT commit_order FROM dolt_log(dolt_hashof('feature')) LIMIT 1) = (SELECT commit_order FROM dolt_log(dolt_hashof('main')) LIMIT 1) AS same_height;",
                    expected: Expected::Rows {
                        columns: &[Column("same_height", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("The script reads the commits' hashes with dolt_hashof, where Dolt's reads them from dolt_commits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature', '-m', 'merge feature');",
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
                    query: "SELECT (SELECT commit_order FROM dolt_log WHERE message = 'merge feature') > (SELECT commit_order FROM dolt_log WHERE message = 'main commit') AS merge_higher;",
                    expected: Expected::Rows {
                        columns: &[Column("merge_higher", BOOL)],
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
            name: "system-tables: query dolt_branches system table",
            intent: "dolt_branches lists each branch with its latest commit and that commit's author and committer.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'create-table-branch');",
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added test table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, latest_commit_message FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_commit_message", TEXT)],
                        rows: &[
                            &[T("create-table-branch"), T("Added test table")],
                            &[T("main"), T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE latest_commit_message = 'CREATE DATABASE';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE latest_commit_message = 'Added test table';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("create-table-branch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_name = 'Bats Committer';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("DOLT_COMMITTER_NAME and DOLT_COMMITTER_EMAIL in the dolt process's environment are the dolt_committer_* settings")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_email = 'committer@email.fake';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '--author', 'Bats Author <author@email.fake>', '-m', 'check author columns');",
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
                    query: "SELECT latest_committer, latest_committer_email, latest_author, latest_author_email FROM dolt_branches WHERE name = 'create-table-branch';",
                    expected: Expected::Rows {
                        columns: &[Column("latest_committer", TEXT), Column("latest_committer_email", TEXT), Column("latest_author", TEXT), Column("latest_author_email", TEXT)],
                        rows: &[
                            &[T("Bats Committer"), T("committer@email.fake"), T("Bats Author"), T("author@email.fake")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_diff system table",
            intent: "dolt_diff lists the staged and working tables' changes, and a commit's author and committer.",
            set_up_script: &[
                "CREATE TABLE testStaged (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('testStaged');",
                "CREATE TABLE testWorking (pk int, c1 int, PRIMARY KEY(pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, data_change, schema_change FROM dolt_diff WHERE commit_hash IN ('STAGED', 'WORKING') ORDER BY commit_hash;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("STAGED"), T("public.teststaged"), T("f"), T("t")],
                            &[T("WORKING"), T("public.testworking"), T("f"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('testWorking');",
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
                    query: "SET dolt_committer_name = 'Bats Committer';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("DOLT_COMMITTER_NAME and DOLT_COMMITTER_EMAIL in the dolt process's environment are the dolt_committer_* settings")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_email = 'committer@email.fake';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--author', 'Bats Author <author@email.fake>', '-m', 'check author columns');",
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
                    query: "SELECT committer, email, author, author_email FROM dolt_diff WHERE message = 'check author columns' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Bats Committer"), T("committer@email.fake"), T("Bats Author"), T("author@email.fake")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_column_diff system table",
            intent: "dolt_column_diff lists the staged and working tables' new columns, and a commit's author and committer.",
            set_up_script: &[
                "CREATE TABLE testStaged (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('testStaged');",
                "CREATE TABLE testWorking (pk int, c1 int, PRIMARY KEY(pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name, column_name, diff_type FROM dolt_column_diff WHERE commit_hash IN ('STAGED', 'WORKING') ORDER BY commit_hash, column_name;",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("STAGED"), T("public.teststaged"), T("c1"), T("added")],
                            &[T("STAGED"), T("public.teststaged"), T("pk"), T("added")],
                            &[T("WORKING"), T("public.testworking"), T("c1"), T("added")],
                            &[T("WORKING"), T("public.testworking"), T("pk"), T("added")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('testWorking');",
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
                    query: "SET dolt_committer_name = 'Bats Committer';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("DOLT_COMMITTER_NAME and DOLT_COMMITTER_EMAIL in the dolt process's environment are the dolt_committer_* settings")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_email = 'committer@email.fake';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--author', 'Bats Author <author@email.fake>', '-m', 'check author columns');",
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
                    query: "SELECT committer, email, author, author_email FROM dolt_column_diff WHERE message = 'check author columns' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Bats Committer"), T("committer@email.fake"), T("Bats Author"), T("author@email.fake")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: dolt_diff author and committer columns are distinct when set differently",
            intent: "dolt_diff shows a commit's separate author and committer.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t_diff_ident (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t_diff_ident');",
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
                    query: "SET dolt_author_name = 'Diff Author';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_author_email = 'diff_author@test.com';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_name = 'Diff Committer';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_email = 'diff_committer@test.com';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'diff identity test');",
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
                    query: "SELECT committer, email, author, author_email FROM dolt_diff WHERE message = 'diff identity test' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Diff Committer"), T("diff_committer@test.com"), T("Diff Author"), T("diff_author@test.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: dolt_column_diff author and committer columns are distinct when set differently",
            intent: "dolt_column_diff shows a commit's separate author and committer.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t_cdiff_ident (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t_cdiff_ident');",
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
                    query: "SET dolt_author_name = 'CDiff Author';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_author_email = 'cdiff_author@test.com';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_name = 'CDiff Committer';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_email = 'cdiff_committer@test.com';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'column diff identity test');",
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
                    query: "SELECT committer, email, author, author_email FROM dolt_column_diff WHERE message = 'column diff identity test' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("CDiff Committer"), T("cdiff_committer@test.com"), T("CDiff Author"), T("cdiff_author@test.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_diff_ system table",
            intent: "dolt_diff_<t> lists each commit's and the working set's row changes, filtered by commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added test table');",
                "SELECT dolt_branch('create_checkpoint', 'main');",
                "INSERT INTO test VALUES (0,0),(1,1),(2,2);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added rows');",
                "SELECT dolt_branch('inserted_rows', 'main');",
                "INSERT INTO test VALUES (3,3);",
                "UPDATE test SET c1=5 WHERE pk=1;",
                "DELETE FROM test WHERE pk=2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff_test ORDER BY dolt_from_commit_date;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), Null, Null, T("added")],
                            &[T("1"), T("1"), Null, Null, T("added")],
                            &[T("2"), T("2"), Null, Null, T("added")],
                            &[T("1"), T("5"), T("1"), T("1"), T("modified")],
                            &[Null, Null, T("2"), T("2"), T("removed")],
                            &[T("3"), T("3"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff_test WHERE dolt_to_commit = 'WORKING' ORDER BY dolt_from_commit_date;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("5"), T("1"), T("1"), T("modified")],
                            &[Null, Null, T("2"), T("2"), T("removed")],
                            &[T("3"), T("3"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff_test WHERE dolt_from_commit = dolt_hashof('create_checkpoint') AND dolt_to_commit = dolt_hashof('inserted_rows') ORDER BY dolt_from_commit_date;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), Null, Null, T("added")],
                            &[T("1"), T("1"), Null, Null, T("added")],
                            &[T("2"), T("2"), Null, Null, T("added")],
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
            name: "system-tables: query dolt_diff_ and dolt_commit_diff_ highlighting differences",
            intent: "dolt_diff_<t> lists single commits' changes while dolt_commit_diff_<t> compares any two revisions, which it needs exactly one of each.",
            set_up_script: &[
                "SELECT dolt_branch('before_creation', 'main');",
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added test table');",
                "SELECT dolt_branch('start', 'main');",
                "INSERT INTO test (pk, c1) VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'add rows 1-3');",
                "UPDATE  test SET c1=4 WHERE pk=2;",
                "UPDATE test SET c1=5 WHERE pk=3;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'modified 2 & 3');",
                "UPDATE test SET c1=2 WHERE pk=2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff_test WHERE dolt_to_commit = dolt_hashof('main') AND dolt_from_commit = dolt_hashof('start') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_to_commit = dolt_hashof('main') AND dolt_from_commit = dolt_hashof('start') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null, T("added")],
                            &[T("2"), T("4"), Null, Null, T("added")],
                            &[T("3"), T("5"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_to_commit = 'WORKING' AND dolt_from_commit = dolt_hashof('start') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null, T("added")],
                            &[T("2"), T("2"), Null, Null, T("added")],
                            &[T("3"), T("5"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_to_commit = 'WORKING' AND dolt_from_commit = dolt_hashof('before_creation') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null, T("added")],
                            &[T("2"), T("2"), Null, Null, T("added")],
                            &[T("3"), T("5"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_to_commit = 'WORKING' AND dolt_from_commit = dolt_hashof('before_creation') AND to_pk = 2 ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("2"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff_test WHERE dolt_to_commit = 'WORKING' AND dolt_from_commit = dolt_hashof('main') ORDER BY to_pk;",
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
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_to_commit = 'WORKING' AND dolt_from_commit = dolt_hashof('main') ORDER BY to_pk;",
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
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_diff_test WHERE dolt_to_commit = dolt_hashof('main') OR dolt_from_commit = dolt_hashof('start') ORDER BY dolt_to_commit_date, to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null, T("added")],
                            &[T("2"), T("2"), Null, Null, T("added")],
                            &[T("3"), T("3"), Null, Null, T("added")],
                            &[T("2"), T("4"), T("2"), T("2"), T("modified")],
                            &[T("3"), T("5"), T("3"), T("3"), T("modified")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, from_pk, from_c1, dolt_diff_type FROM dolt_commit_diff_test WHERE dolt_to_commit = dolt_hashof('main') OR dolt_from_commit = dolt_hashof('start') ORDER BY to_pk;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "must be filtered to a single", message_contains: true, ..E }),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_diff_ system table without committing table",
            intent: "dolt_diff_<t> of a table never committed lists its working rows as added.",
            set_up_script: &[
                "CREATE TABLE test (pk int NOT NULL PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, dolt_to_commit, from_pk, dolt_diff_type FROM dolt_diff_test ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("dolt_to_commit", TEXT), Column("from_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("WORKING"), Null, T("added")],
                            &[T("1"), T("WORKING"), Null, T("added")],
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
            name: "system-tables: query dolt_diff_ system table when column types have been narrowed",
            intent: "dolt_diff_<t> across columns narrowed after a row was written reads the old values that no longer fit as NULL.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(20), col2 int);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "INSERT INTO t VALUES (1, '123456789012345', 420);",
                "SELECT dolt_commit('-am', 'inserting a row');",
                "UPDATE t SET col1='1234567890', col2=13;",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(10);",
                "ALTER TABLE t ALTER COLUMN col2 TYPE smallint;",
                "SELECT dolt_commit('-am', 'narrowing types');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_col1, to_col2, from_pk, from_col1, from_col2, dolt_diff_type FROM dolt_diff_t ORDER BY dolt_from_commit_date ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_col1", VARCHAR), Column("to_col2", INT2), Column("from_pk", INT4), Column("from_col1", VARCHAR), Column("from_col2", INT2), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("420"), Null, Null, Null, T("added")],
                            &[T("1"), T("1234567890"), T("13"), T("1"), Null, T("420"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("MySQL's tinyint can't hold 420, so Dolt reads it as NULL, where Postgres' smallint, which stands for it, holds it. Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col1' into latest column schema", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "unable to coerce value from field 'col1' into latest column schema", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_history_ system table",
            intent: "dolt_history_<t> lists each commit's rows, for tables with and without primary keys.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added test table');",
                "INSERT INTO test VALUES (0,0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added (0,0) row');",
                "INSERT INTO test VALUES (1,1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added (1,1) row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_history_test;",
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
                    query: "SELECT count(*) FROM dolt_history_test WHERE pk=1;",
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
                    query: "SELECT count(*) FROM dolt_history_test WHERE pk=0;",
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
                    query: "CREATE TABLE nopks (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO nopks VALUES (123, 'onetwothree'), (234, 'twothreefour');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('nopks');",
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
                    query: "SELECT dolt_commit('-m', 'Adding table nopks');",
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
                    query: "SELECT count(*) FROM dolt_history_nopks;",
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
                    query: "SELECT b FROM dolt_history_nopks WHERE a=123;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("onetwothree")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_commits",
            intent: "dolt_commits lists every branch's commits with their author and committer.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commits;",
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
                    query: "CREATE TABLE test (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
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
                    query: "SELECT dolt_commit('-m', 'added table test');",
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
                    query: "SELECT dolt_branch('other');",
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
                    query: "INSERT INTO test VALUES (0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
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
                    query: "SELECT dolt_commit('-m', 'added values to test on main');",
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
                    query: "SELECT * FROM dolt_checkout('other');",
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
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
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
                    query: "SELECT dolt_commit('-m', 'added values to test on other');",
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
                    query: "SELECT count(*) FROM dolt_commits;",
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
                    query: "SET dolt_committer_name = 'Bats Committer';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("DOLT_COMMITTER_NAME and DOLT_COMMITTER_EMAIL in the dolt process's environment are the dolt_committer_* settings")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_email = 'committer@email.fake';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '--author', 'Bats Author <author@email.fake>', '-m', 'check author columns');",
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
                    query: "SELECT committer, email, author, author_email FROM dolt_commits WHERE message = 'check author columns' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Bats Committer"), T("committer@email.fake"), T("Bats Author"), T("author@email.fake")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_ancestor_commits",
            intent: "dolt_commit_ancestors lists each commit's parents, a NULL one for the initial commit and two for a merge.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit_ancestors;",
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
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE parent_hash IS NULL;",
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
                    query: "CREATE TABLE test (pk int PRIMARY KEY);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit B');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit C');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge other (commit M)');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit_ancestors;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: join dolt_commits and dolt_commit_ancestors",
            intent: "Joining dolt_commits to dolt_commit_ancestors names each parent and its index.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit A');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (0);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit B');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit C');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other (commit M)');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT an.parent_index, cm.message FROM dolt_commits AS cm JOIN dolt_commit_ancestors AS an ON cm.commit_hash = an.parent_hash ORDER BY cm.date, cm.message;",
                    expected: Expected::Rows {
                        columns: &[Column("parent_index", INT4), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Initialize data repository")],
                            &[T("0"), T("commit A")],
                            &[T("0"), T("commit A")],
                            &[T("0"), T("commit B")],
                            &[T("1"), T("commit C")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: dolt_branches is read-only",
            intent: "dolt_branches refuses deletes, inserts, and updates.",
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branches;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot delete from view \"dolt_branches\"", detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's dolt_branches is a view, which Postgres refuses to write as it refuses any view that isn't updatable, where Dolt calls the table read-only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branches (name,hash) VALUES ('branch1', 'main');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot insert into view \"dolt_branches\"", detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's dolt_branches is a view, which Postgres refuses to write as it refuses any view that isn't updatable, where Dolt calls the table read-only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branches SET name = 'branch1' WHERE name = 'main';",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot update view \"dolt_branches\"", detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule.", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's dolt_branches is a view, which Postgres refuses to write as it refuses any view that isn't updatable, where Dolt calls the table read-only")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: dolt diff includes changes from initial commit",
            intent: "dolt_diff_<t> includes the rows of the table's first commit.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY, val int);",
                "INSERT INTO test VALUES (1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm1');",
                "INSERT INTO test VALUES (2,2);",
                "SELECT dolt_commit('-am', 'cm2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_val, to_pk FROM dolt_diff_test ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_val", INT4), Column("to_pk", INT4)],
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
            name: "system-tables: query dolt_tags",
            intent: "dolt_tags lists every tag with its message.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (1,1);",
                "SELECT dolt_commit('-am', 'cm1');",
                "SELECT dolt_tag('v1', 'head', '-m', 'tag v1 from main');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO test VALUES (2,2);",
                "SELECT dolt_commit('-am', 'cm2');",
                "SELECT dolt_tag('v2', 'branch1~', '-m', 'tag v2 from branch1');",
                "SELECT dolt_tag('v3', 'branch1', '-m', 'tag v3 from branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_tags ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("tag v1 from main")],
                            &[T("tag v2 from branch1")],
                            &[T("tag v3 from branch1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_schema_diff",
            intent: "dolt_schema_diff shows each side's statement for a table whose columns changed between two revisions, in either order, and nothing for a revision against itself.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'commit 1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('tag1');",
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
                    query: "SELECT * FROM dolt_checkout('-b', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN c2, ADD COLUMN c3 varchar(10);",
                    expected: Expected::Tag("ALTER TABLE"),
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
                    query: "SELECT dolt_commit('-m', 'commit 2');",
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
                    query: "SELECT dolt_tag('tag2');",
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
                    query: "SELECT * FROM dolt_checkout('-b', 'branch3');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch3'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "SELECT dolt_commit('-m', 'commit 3');",
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
                    query: "SELECT dolt_tag('tag3');",
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
                    query: "SELECT from_table_name, to_table_name, from_create_statement LIKE '%c2%', to_create_statement LIKE '%c3%' FROM dolt_schema_diff('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_table_name, to_table_name, from_create_statement LIKE '%c3%', to_create_statement LIKE '%c2%' FROM dolt_schema_diff('branch2', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_table_name, to_table_name, from_create_statement LIKE '%c2%', to_create_statement LIKE '%c3%' FROM dolt_schema_diff('tag1', 'tag2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_table_name, to_table_name, from_create_statement LIKE '%c3%', to_create_statement LIKE '%c2%' FROM dolt_schema_diff('tag2', 'tag1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_diff('branch1', 'branch1');",
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
                    query: "SELECT count(*) FROM dolt_schema_diff('tag2', 'tag2');",
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
            name: "system-tables: query dolt_help system table",
            intent: "dolt_help describes Dolt's procedures: their type, synopsis, descriptions, and arguments.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT type FROM dolt_help WHERE name = 'dolt_rebase';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("procedure")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT synopsis LIKE '%dolt backup [-v | --verbose]%' FROM dolt_help WHERE name = 'dolt_backup';",
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
                    query: "SELECT synopsis LIKE '%restore [--aws-region <region>] [--aws-creds-type <creds-type>] [--aws-creds-file <file>] [--aws-creds-profile <profile>] [--force] <url> <name>%' FROM dolt_help WHERE name = 'dolt_backup';",
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
                    query: "SELECT short_description LIKE '%Record changes to the database%' FROM dolt_help WHERE name = 'dolt_commit';",
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
                    query: "SELECT long_description LIKE '%This command updates the list of tables using the current content found in the working root%' FROM dolt_help WHERE name = 'dolt_add';",
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
                    query: "SELECT long_description LIKE '%This command can be performed multiple times before a commit.%' FROM dolt_help WHERE name = 'dolt_add';",
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
                    query: "SELECT arguments::text LIKE '%The name of the remote to pull from.%' FROM dolt_help WHERE name = 'dolt_pull';",
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
                    query: "SELECT arguments::text LIKE '%The name of a branch on the specified remote to be merged into the current working set.%' FROM dolt_help WHERE name = 'dolt_pull';",
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
                    query: "SELECT arguments::text LIKE '%Update from the remote HEAD even if there are errors.%' FROM dolt_help WHERE name = 'dolt_pull';",
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
                    query: "SELECT arguments::text LIKE '%Use the given msg as the tag message.%' FROM dolt_help WHERE name = 'dolt_tag';",
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
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
        ScriptTest {
            name: "system-tables: dolt_log table function with prepared statements",
            intent: "dolt_log takes its revisions and flags from a prepared statement's parameters.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "CREATE TABLE test2 (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test2');",
                "SELECT dolt_commit('-m', 'second commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE stmt1 AS SELECT count(*) FROM dolt_log($1);",
                    expected: Expected::Tag("PREPARE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt1('HEAD');",
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
                    query: "PREPARE stmt2 AS SELECT count(*) FROM dolt_log($1);",
                    expected: Expected::Tag("PREPARE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt2('HEAD~1..HEAD');",
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
                    query: "PREPARE stmt3 AS SELECT count(*) FROM dolt_log($1, '--not', $2);",
                    expected: Expected::Tag("PREPARE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt3('HEAD', 'HEAD~1');",
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
                    query: "PREPARE stmt4 AS SELECT count(*) AS n FROM dolt_log($1, '--parents');",
                    expected: Expected::Tag("PREPARE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt4('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE stmt5 AS SELECT count(*) FILTER (WHERE parents = '') AS empty_count FROM dolt_log($1, '--parents');",
                    expected: Expected::Tag("PREPARE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt5('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("empty_count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL sums the comparison's 0 and 1, which Postgres counts with FILTER")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE stmt6 AS SELECT count(*) FROM dolt_log($1, '--not', 'HEAD~2');",
                    expected: Expected::Tag("PREPARE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt6('HEAD');",
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
                    query: "PREPARE stmt7 AS SELECT count(*) AS n FROM dolt_log('HEAD', $1);",
                    expected: Expected::Tag("PREPARE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt7('--parents');",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE stmt_fail AS SELECT count(*) FILTER (WHERE parents = '') AS empty_count FROM dolt_log('HEAD', $1);",
                    expected: Expected::Tag("PREPARE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt_fail('--parents');",
                    expected: Expected::Rows {
                        columns: &[Column("empty_count", INT8)],
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
            name: "system-tables: dolt_diff bind variable rejection - dynamic table function",
            intent: "dolt_diff, whose columns depend on its table, refuses its revisions as prepared statement parameters.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(100));",
                "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');",
                "SELECT dolt_add('users');",
                "SELECT dolt_commit('-m', 'Added users table');",
                "INSERT INTO users VALUES (3, 'Charlie');",
                "SELECT dolt_add('users');",
                "SELECT dolt_commit('-m', 'Added Charlie');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE stmt_diff AS SELECT * FROM dolt_diff($1, $2, 'users');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_diff:", message_contains: true, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's PREPARE ... FROM and EXECUTE ... USING are Postgres' PREPARE ... AS and EXECUTE with arguments")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: dolt_query_diff bind variable rejection - dynamic table function",
            intent: "dolt_query_diff, whose columns depend on its queries, refuses them as prepared statement parameters.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(100));",
                "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');",
                "SELECT dolt_add('users');",
                "SELECT dolt_commit('-m', 'Added users table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE stmt_query_diff AS SELECT * FROM dolt_query_diff($1, $2);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "a column definition list is required for functions returning \"record\"", position: 42, ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's dolt_query_diff returns records whose columns its queries decide, so Postgres asks for a column definition list when the queries are parameters, where Dolt names the unbound variable")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: dolt_preview_merge_conflicts bind variable support - dynamic table function",
            intent: "dolt_preview_merge_conflicts takes its branches and table as prepared statement parameters.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(100));",
                "INSERT INTO users VALUES (1, 'Alice');",
                "SELECT dolt_add('users');",
                "SELECT dolt_commit('-m', 'Added users table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "UPDATE users SET name = 'Bob' WHERE id = 1;",
                "SELECT dolt_add('users');",
                "SELECT dolt_commit('-m', 'Changed Alice to Bob');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE users SET name = 'Charlie' WHERE id = 1;",
                "SELECT dolt_add('users');",
                "SELECT dolt_commit('-m', 'Changed Alice to Charlie');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE stmt_preview AS SELECT dolt_conflict_id IS NOT NULL FROM dolt_preview_merge_conflicts($1, $2, $3);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_preview_merge_conflicts:", message_contains: true, ..E }),
                    dolt: Some(Dolt::Differs("Postgres analyzes a prepared statement before its parameters have values, Appendix C \"dolt_diff() arguments in prepared statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE stmt_preview('other', 'main', 'users');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("the PREPARE before it fails: Postgres analyzes a prepared statement before its parameters have values, Appendix C \"dolt_diff() arguments in prepared statements\""),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_status_ignored system table",
            intent: "dolt_status_ignored lists ignored tables beside the others and marks them, which dolt_status leaves out.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added test table');",
                "INSERT INTO dolt_ignore VALUES ('ignored_*', TRUE);",
                "CREATE TABLE ignored_table (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_status WHERE staged = false ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("dolt_ignore")],
                            &[T("public.test")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE staged = false ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("dolt_ignore"), T("f")],
                            &[T("public.ignored_table"), T("t")],
                            &[T("public.test"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE table_name = 'public.ignored_table' ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.ignored_table"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE table_name = 'public.test' ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.test"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: dolt_status_ignored shows staged tables without ignored flag",
            intent: "dolt_status_ignored never marks a staged table ignored.",
            set_up_script: &[
                "CREATE TABLE staged_test (pk int PRIMARY KEY);",
                "SELECT dolt_add('staged_test');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored WHERE staged = true AND table_name = 'public.staged_test' ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.staged_test"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_remote_branches system table",
            intent: "dolt_remote_branches lists remote-tracking branches but no local ones, filters on their commit messages, unions with dolt_branches, and is empty in a database without remotes.",
            set_up_script: &[
                "SELECT dolt_checkout('-b', 'create-table-branch');",
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added test table');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_remote('add', 'rem1', 'file://{NEWDIR:remote1}');",
                "SELECT * FROM dolt_push('rem1', 'b1');",
                "SELECT dolt_branch('-d', 'b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, latest_commit_message FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_commit_message", TEXT)],
                        rows: &[
                            &[T("create-table-branch"), T("Added test table")],
                            &[T("main"), T(r#"Initialize data repository"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, latest_commit_message FROM dolt_remote_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("latest_commit_message", TEXT)],
                        rows: &[
                            &[T("remotes/rem1/b1"), T("Added test table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_remote_branches WHERE latest_commit_message = 'Initialize data repository';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_remote_branches WHERE latest_commit_message = 'Added test table';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("remotes/rem1/b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches UNION SELECT name FROM dolt_remote_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("create-table-branch")],
                            &[T("main")],
                            &[T("remotes/rem1/b1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE noremotes;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t1(a int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "noremotes",
                    database: "noremotes",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'new table') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "noremotes",
                    database: "noremotes",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "noremotes",
                    database: "noremotes",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_remote_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "noremotes",
                    database: "noremotes",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: query dolt_remotes system table",
            intent: "dolt_remotes lists a remote dolt_remote adds, with its fetch specs, parameters, and absolute URL.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_remotes;",
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
                    query: "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
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
                    query: "SELECT count(*) FROM dolt_remotes;",
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
                    query: "SELECT name, fetch_specs, params FROM dolt_remotes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("fetch_specs", JSONB), Column("params", JSONB)],
                        rows: &[
                            &[T("origin"), T("[\"refs/heads/*:refs/remotes/origin/*\"]"), T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT url FROM dolt_remotes;",
                    expected: Expected::Rows {
                        columns: &[Column("url", TEXT)],
                        rows: &[
                            &[T("file://{NEWDIR:remote}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: check unsupported dolt_remote behavior",
            intent: "dolt_remotes refuses inserts and deletes.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_remotes (name, url) VALUES ('origin', 'file://remote');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot insert into view \"dolt_remotes\"", detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.", ..E }),
                    dolt: Some(Dolt::Differs("dolt_remotes is a view over the database's remotes, so writes fail as Postgres refuses them on a view, where Dolt's table refuses them with its own message")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
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
                    query: "DELETE FROM dolt_remotes WHERE name = 'origin';",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot delete from view \"dolt_remotes\"", detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.", ..E }),
                    dolt: Some(Dolt::Differs("dolt_remotes is a view over the database's remotes, so writes fail as Postgres refuses them on a view, where Dolt's table refuses them with its own message")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: insert into dolt_remotes system table using dolt_remote procedure",
            intent: "dolt_remotes lists the file and AWS remotes dolt_remote adds.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin1', 'file://{NEWDIR:remote}');",
                "SELECT dolt_remote('add', 'origin2', 'aws://[dynamo_db_table:s3_bucket]/repo_name');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_remotes;",
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
                    query: "SELECT name, fetch_specs, params FROM dolt_remotes ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("fetch_specs", JSONB), Column("params", JSONB)],
                        rows: &[
                            &[T("origin1"), T("[\"refs/heads/*:refs/remotes/origin1/*\"]"), T("{}")],
                            &[T("origin2"), T("[\"refs/heads/*:refs/remotes/origin2/*\"]"), T("{}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT url FROM dolt_remotes ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("url", TEXT)],
                        rows: &[
                            &[T("file://{NEWDIR:remote}")],
                            &[T("aws://[dynamo_db_table:s3_bucket]/repo_name")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "system-tables: delete from dolt_remotes system table using dolt_remote procedure",
            intent: "dolt_remote('remove', ...) drops a remote from dolt_remotes, and refuses an unknown one.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_remotes;",
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
                    query: "SELECT name, fetch_specs, params FROM dolt_remotes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("fetch_specs", JSONB), Column("params", JSONB)],
                        rows: &[
                            &[T("origin"), T("[\"refs/heads/*:refs/remotes/origin/*\"]"), T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT url FROM dolt_remotes;",
                    expected: Expected::Rows {
                        columns: &[Column("url", TEXT)],
                        rows: &[
                            &[T("file://{NEWDIR:remote}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('remove', 'origin1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: unknown remote: 'origin1'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_remotes;",
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
                    query: "SELECT dolt_remote('remove', 'origin');",
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
                    query: "SELECT count(*) FROM dolt_remotes;",
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
            name: "system-tables: revision databases can query dolt_remotes table",
            intent: "A branch's revision database lists the database's remotes.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_remotes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("origin")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_remotes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("origin")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/b1",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
