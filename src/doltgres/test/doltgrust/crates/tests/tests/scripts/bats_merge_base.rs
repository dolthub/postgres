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

//! Dolt's integration-tests/bats/merge-base.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_merge_base() {
    run_scripts(&[
        ScriptTest {
            name: "merge-base: cli",
            intent: "The merge base of two branches is the same commit from every pair, in both branches' logs and not in an older branch's.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit A');",
                "SELECT dolt_branch('zero');",
                "INSERT INTO test VALUES (0);",
                "SELECT dolt_commit('-am', 'commit B');",
                "SELECT dolt_branch('one');",
                "SELECT dolt_branch('two');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_commit('-am', 'commit C');",
                "SELECT * FROM dolt_checkout('two');",
                "INSERT INTO test VALUES (2);",
                "SELECT dolt_commit('-am', 'commit D');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_merge_base('main', 'one') = dolt_merge_base('main', 'two') AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_merge_base('one', 'two') = dolt_merge_base('main', 'two') AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_log('main') WHERE commit_hash = dolt_merge_base('main', 'two')) AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_log('two') WHERE commit_hash = dolt_merge_base('main', 'two')) AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_log('zero') WHERE commit_hash = dolt_merge_base('main', 'two')) AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
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
            name: "merge-base: sql",
            intent: "dolt_merge_base finds the newest common ancestor of two revisions, by branch name or hash.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit A');",
                "SELECT dolt_branch('zero');",
                "INSERT INTO test VALUES (0);",
                "SELECT dolt_commit('-am', 'commit B');",
                "SELECT dolt_branch('one');",
                "SELECT dolt_branch('two');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_commit('-am', 'commit C');",
                "SELECT * FROM dolt_checkout('two');",
                "INSERT INTO test VALUES (2);",
                "SELECT dolt_commit('-am', 'commit D');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log WHERE commit_hash = dolt_merge_base('main', 'zero');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit A")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log WHERE commit_hash = dolt_merge_base('main', 'one');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit B")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log WHERE commit_hash = dolt_merge_base('main', 'two');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit B")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log WHERE commit_hash = dolt_merge_base('main', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("commit C")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_merge_base('main', dolt_hashof('one')) = dolt_merge_base(dolt_hashof('main'), 'one') AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
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
            name: "merge-base: --all",
            intent: "dolt_merge_bases lists the one merge base of two branches, and both bases of a criss-cross merge, the newest first.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit A');",
                "SELECT dolt_branch('zero');",
                "INSERT INTO test VALUES (0);",
                "SELECT dolt_commit('-am', 'commit B');",
                "SELECT dolt_branch('one');",
                "SELECT dolt_branch('two');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_commit('-am', 'commit C');",
                "SELECT * FROM dolt_checkout('two');",
                "INSERT INTO test VALUES (2);",
                "SELECT dolt_commit('-am', 'commit D');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'branchA');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'baseA', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('-b', 'branchB', 'main');",
                "INSERT INTO t VALUES (20);",
                "SELECT dolt_commit('-am', 'baseB', '--date', '2022-08-06T12:00:02');",
                "SELECT * FROM dolt_merge('branchA~0', '-m', 'mergeB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT * FROM dolt_merge('branchB~1', '-m', 'mergeA');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT merge_base = dolt_merge_base('main', 'two') AS found FROM dolt_merge_bases('main', 'two');",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.message FROM (SELECT merge_base, ROW_NUMBER() OVER () AS rn FROM dolt_merge_bases('branchA', 'branchB')) b JOIN dolt_log('branchA') l ON l.commit_hash = b.merge_base ORDER BY b.rn;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("baseB")],
                            &[T("baseA")],
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
