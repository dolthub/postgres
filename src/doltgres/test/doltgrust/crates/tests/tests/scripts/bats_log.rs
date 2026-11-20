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

//! Dolt's integration-tests/bats/log.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_log() {
    run_scripts(&[
        ScriptTest {
            name: "log: on initialized repo",
            intent: "A new database's log holds its initial commit.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: basic log",
            intent: "The log lists every commit.",
            set_up_script: &[
                "CREATE TABLE testtable (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit 1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 2');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1")],
                            &[T("commit 2")],
                            &[T("commit 3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: log respects branches",
            intent: "The log lists the checked-out branch's commits only.",
            set_up_script: &[
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 MAIN');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 2 MAIN');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 3 MAIN');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 MAIN")],
                            &[T("commit 2 MAIN")],
                            &[T("commit 3 MAIN")],
                        ],
                        tag: "SELECT 4",
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
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 BRANCH1');",
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
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'commit 2 BRANCH1');",
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
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'commit 3 BRANCH1');",
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
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 BRANCH1")],
                            &[T("commit 2 BRANCH1")],
                            &[T("commit 3 BRANCH1")],
                        ],
                        tag: "SELECT 4",
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
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 MAIN")],
                            &[T("commit 2 MAIN")],
                            &[T("commit 3 MAIN")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: two and three dot log",
            intent: "Two-dot, three-dot, ^, and --not ranges of dolt_log list the commits of each range, and mixing a range with other revisions is refused.",
            set_up_script: &[
                "CREATE TABLE testtable (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit 1 MAIN');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 2 MAIN');",
                "SELECT * FROM dolt_checkout('-b', 'branchA');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 BRANCHA');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 2 BRANCHA');",
                "SELECT * FROM dolt_checkout('-b', 'branchB');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 BRANCHB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 3 BRANCHA');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 1 MAIN")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 2 MAIN")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 MAIN")],
                            &[T("commit 2 MAIN")],
                        ],
                        tag: "SELECT 3",
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
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'commit 3 AFTER');",
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
                    query: "SELECT message FROM dolt_log('main..branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('^main', 'branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchA', '^main') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchA', '--not', 'main') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchA..main') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 3 AFTER")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main..main') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main^..branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('^main^', 'branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchA', '--not', 'main^') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('^main', '^branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main...branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 AFTER")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchA...main') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 AFTER")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main', 'branchA', '--not', dolt_merge_base('main', 'branchA')) ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 3 AFTER")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("The CLI's $(dolt merge-base main branchA) is dolt_merge_base")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB...branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 1 BRANCHB")],
                            &[T("commit 1 MAIN")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 2 MAIN")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main', 'branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 1 MAIN")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 2 MAIN")],
                            &[T("commit 3 AFTER")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main', 'branchB', 'branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 BRANCHA")],
                            &[T("commit 1 BRANCHB")],
                            &[T("commit 1 MAIN")],
                            &[T("commit 2 BRANCHA")],
                            &[T("commit 2 MAIN")],
                            &[T("commit 3 AFTER")],
                            &[T("commit 3 BRANCHA")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'main', '^branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB")],
                            &[T("commit 3 AFTER")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'main', '--not', 'branchA') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB")],
                            &[T("commit 3 AFTER")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'main', '^branchA', '^main') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchB', 'main', '--not', 'branchA', 'main') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit 1 BRANCHB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main..branchA', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main', 'main..branchA');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main...branchA', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main', 'main...branchA');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main..branchA', '--not', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main..branchA', '--not', '^main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main...branchA', '--not', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('main...branchA', '--not', '^main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: branch with multiple tables",
            intent: "dolt_log of a branch restricted to tables lists only the commits that changed them.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test');",
                "CREATE TABLE test2 (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test2');",
                "SELECT * FROM dolt_checkout('-b', 'b1');",
                "INSERT INTO test VALUES (0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'inserted 0 into test');",
                "CREATE TABLE test3 (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test3');",
                "SELECT * FROM dolt_checkout('-b', 'b2');",
                "INSERT INTO test2 VALUES (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'inserted 1 into test2');",
                "CREATE TABLE test4 (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test4');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('b1', '--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("created table test")],
                            &[T("inserted 0 into test")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('b1', '--tables', 'test,test2') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("created table test")],
                            &[T("created table test2")],
                            &[T("inserted 0 into test")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('b2', 'b1', '--tables', 'test,test2') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("created table test")],
                            &[T("created table test2")],
                            &[T("inserted 0 into test")],
                            &[T("inserted 1 into test2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: with -n specified",
            intent: "dolt_log with -n 1 lists only the newest commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'first commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("first commit")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '1') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: on fast-forward merge commits",
            intent: "A fast-forward merge adds the branch's commits to the log without a merge commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit1');",
                "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test VALUES (0,0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('test-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                            &[T("Commit2")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: properly orders merge commits",
            intent: "An uncommitted merge leaves the log alone, and the merge commit then lists both parents' commits, newest first.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit1');",
                "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test VALUES (0,0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (1,1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test-branch', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, Any, T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                            &[T("Commit3")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
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
                    query: "SELECT dolt_commit('-m', 'MergeCommit');",
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
                    query: "SELECT message FROM dolt_log() ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                            &[T("Commit2")],
                            &[T("Commit3")],
                            &[T("Initialize data repository")],
                            &[T("MergeCommit")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '5') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                            &[T("Commit2")],
                            &[T("Commit3")],
                            &[T("Initialize data repository")],
                            &[T("MergeCommit")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '4') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                            &[T("Commit2")],
                            &[T("Commit3")],
                            &[T("MergeCommit")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '3') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit2")],
                            &[T("Commit3")],
                            &[T("MergeCommit")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '2') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit3")],
                            &[T("MergeCommit")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '1') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("MergeCommit")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: Properly throws an error when neither a valid commit hash nor a valid table are passed",
            intent: "dolt_log of a name that is no revision is refused.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('notvalid');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: notvalid", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: Log on a table has basic functionality",
            intent: "dolt_log restricted to a table lists only the commits that changed it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'first commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
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
                    query: "SELECT dolt_commit('-am', 'second commit');",
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
                    query: "SELECT message FROM dolt_log('--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
                            &[T("second commit")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
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
                    query: "SELECT dolt_commit('-am', 'third commit');",
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
                    query: "SELECT message FROM dolt_log('--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
                            &[T("second commit")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: Log on a table works with -n",
            intent: "dolt_log restricted to a table and a count lists that many of the commits that changed it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'first commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '1', '--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
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
                    query: "SELECT dolt_commit('-am', 'second commit');",
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
                    query: "SELECT message FROM dolt_log('-n', '2', '--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
                            &[T("second commit")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
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
                    query: "SELECT dolt_commit('-am', 'third commit');",
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
                    query: "INSERT INTO test2 VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'fourth commit');",
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
                    query: "SELECT message FROM dolt_log('-n', '1', '--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("second commit")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '100', '--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
                            &[T("second commit")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: Log on a table works with merge commits",
            intent: "dolt_log restricted to a table lists a merge's commits that changed it once the merge is committed.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit1');",
                "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test VALUES (0,0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (1,1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test-branch', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, Any, T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                            &[T("Commit3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
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
                    query: "SELECT dolt_commit('-m', 'MergeCommit');",
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
                    query: "SELECT message FROM dolt_log('--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                            &[T("Commit2")],
                            &[T("Commit3")],
                            &[T("MergeCommit")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: dolt log with ref and table",
            intent: "dolt_log of another branch restricted to a table lists that branch's commits that changed it.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit1');",
                "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test VALUES (0,0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('test-branch', '--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit1")],
                            &[T("Commit2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: dolt log with table deleted between revisions",
            intent: "dolt_log restricted to a table that was dropped and recreated lists the commits that created, dropped, and recreated it.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit1');",
                "DROP TABLE test;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit 2');",
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit 2")],
                            &[T("Commit1")],
                            &[T("Commit3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: --merges, --parents, --min-parents option",
            intent: "--merges and --min-parents 2 list only merge commits, --min-parents 1 leaves out the initial commit, and --parents names each commit's parents.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY(pk));",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'Created table');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO test VALUES (0,0);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'Inserted 0,0');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "INSERT INTO test VALUES (1,1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'Inserted 1,1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
                "SELECT * FROM dolt_merge('branch2', '-m', 'Merged branch2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--merges') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Merged branch2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--min-parents', '2') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Merged branch2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--min-parents', '1') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Created table")],
                            &[T("Inserted 0,0")],
                            &[T("Inserted 1,1")],
                            &[T("Merged branch2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--parents') WHERE parents LIKE '%, %';",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Merged branch2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: --decorate=short shows trimmed branches and tags",
            intent: "Short decoration names the head's branch and tag without their ref prefixes.",
            set_up_script: &[
                "SELECT dolt_tag('tag_v0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log('--decorate', 'short') WHERE commit_hash = dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> main, tag: tag_v0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: --decorate=full shows full branches and tags",
            intent: "Full decoration names the head's branch and tag with their ref prefixes.",
            set_up_script: &[
                "SELECT dolt_tag('tag_v0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log('--decorate', 'full') WHERE commit_hash = dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> refs/heads/main, tag: refs/tags/tag_v0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: --decorate=no doesn't show branches or tags",
            intent: "Without decoration the refs column is empty.",
            set_up_script: &[
                "SELECT dolt_tag('tag_v0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log('--decorate', 'no') WHERE commit_hash = dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: --decorate=notanoption throws error",
            intent: "dolt_log refuses an unknown decoration.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_log('--decorate', 'notanoption');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_log: ", message_contains: true, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: string formatting characters are escaped",
            intent: "A commit message with a percent sign keeps it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', '% should be escaped');",
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
                    query: "SELECT message FROM dolt_log('-n', '1') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("% should be escaped")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: identify HEAD",
            intent: "Decoration marks HEAD on the head commit only.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1');",
                "SELECT dolt_tag('commit1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 2');",
                "SELECT dolt_tag('commit2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log('--decorate', 'short', 'commit1') WHERE commit_hash = dolt_hashof('commit1');",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> tag: commit1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_log marks its starting commit as HEAD, as Dolt's dolt_log table function does, where the bats check reads the CLI, which looks HEAD up itself")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refs FROM dolt_log('--decorate', 'short', 'commit2') WHERE commit_hash = dolt_hashof('commit2');",
                    expected: Expected::Rows {
                        columns: &[Column("refs", TEXT)],
                        rows: &[
                            &[T("HEAD -> main, tag: commit2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: --all correctly gets branches",
            intent: "dolt_log with --all lists every branch's commits.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 br1');",
                "SELECT dolt_branch('br1');",
                "SELECT dolt_reset('--hard', 'HEAD~1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 br2');",
                "SELECT dolt_branch('br2');",
                "SELECT dolt_reset('--hard', 'HEAD~1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--all') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("commit 1 br1")],
                            &[T("commit 1 br2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: --all works when specifying tables",
            intent: "dolt_log with --all restricted to a table lists every branch's commits that changed it.",
            set_up_script: &[
                "CREATE TABLE test (i int PRIMARY KEY);",
                "SELECT dolt_commit('-A', '-m', 'A table for br1');",
                "SELECT dolt_branch('br1');",
                "SELECT dolt_reset('--hard', 'HEAD~1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'commit 1 br2');",
                "SELECT dolt_branch('br2');",
                "SELECT dolt_reset('--hard', 'HEAD~1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('--all', '--tables', 'test') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("A table for br1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "log: A...B with several merge bases leaves out every merge base",
            intent: "A three-dot log of branches with two merge bases lists only the two merge commits.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'branchA');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'baseA');",
                "SELECT set_config('test.base_a', dolt_hashof('HEAD'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branchB', 'main');",
                "INSERT INTO t VALUES (20);",
                "SELECT pg_sleep(1);",
                "SELECT dolt_commit('-am', 'baseB');",
                "SELECT set_config('test.base_b', dolt_hashof('HEAD'), FALSE);",
                "SELECT * FROM dolt_merge(current_setting('test.base_a'), '-m', 'mergeB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT * FROM dolt_merge(current_setting('test.base_b'), '-m', 'mergeA');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchA...branchB') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("mergeA")],
                            &[T("mergeB")],
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
