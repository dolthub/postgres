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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_squash_history.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_squash_history_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_squash_history: default collapses history to init <- HEAD",
            intent: "Without --first, dolt_squash_history replaces the history after the initial commit with one commit holding HEAD's tree and the message.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'c3');",
                "SELECT set_config('test.init', ((SELECT commit_hash FROM dolt_log WHERE message = 'Initialize data repository'))::text, FALSE);",
                "SELECT set_config('test.user', ((SELECT committer FROM dolt_log WHERE message = 'c1'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
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
                    query: "SELECT dolt_squash_history('--message', 'squashed');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_squash_history", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
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
                            &[T("squashed")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE commit_hash = current_setting('test.init', TRUE);",
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
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'squashed' AND committer = current_setting('test.user', TRUE);",
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
                    query: "SELECT * FROM t ORDER BY pk;",
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
                    query: "SELECT count(*) FROM dolt_status;",
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
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: explicit --first collapses a suffix",
            intent: "dolt_squash_history --first collapses only the commits from that one to HEAD into one.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'c3');",
                "INSERT INTO t VALUES (4);",
                "SELECT dolt_commit('-am', 'c4');",
                "SELECT set_config('test.c3', ((SELECT commit_hash FROM dolt_log WHERE message = 'c3'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'sq', '--first', current_setting('test.c3', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_squash_history", TEXT)],
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
                            &[T("sq")],
                            &[T("c2")],
                            &[T("c1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: --first accepts a relative ref (HEAD~N)",
            intent: "dolt_squash_history --first takes a relative revision such as HEAD~N.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'c3');",
                "INSERT INTO t VALUES (4);",
                "SELECT dolt_commit('-am', 'c4');",
                "INSERT INTO t VALUES (5);",
                "SELECT dolt_commit('-am', 'c5');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'sq', '--first', 'HEAD~4');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_squash_history", TEXT)],
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
                            &[T("sq")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: --first accepts a branch name",
            intent: "dolt_squash_history --first takes a branch name.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'c3');",
                "SELECT set_config('test.c1', ((SELECT commit_hash FROM dolt_log WHERE message = 'c1'))::text, FALSE);",
                "SELECT dolt_branch('base', current_setting('test.c1', TRUE));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'sq', '--first', 'base');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_squash_history", TEXT)],
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
                            &[T("sq")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log AS OF 'base';",
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
                    query: "SELECT * FROM t ORDER BY pk;",
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
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: --first accepts a tag",
            intent: "dolt_squash_history --first takes a tag.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'c3');",
                "SELECT set_config('test.c1', ((SELECT commit_hash FROM dolt_log WHERE message = 'c1'))::text, FALSE);",
                "SELECT dolt_tag('v1', current_setting('test.c1', TRUE));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'sq', '--first', 'v1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_squash_history", TEXT)],
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
                            &[T("sq")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
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
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: --first that is a merge commit keeps both parents",
            intent: "When --first names a merge commit, the squashed commit keeps both of its parents.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'A');",
                "SELECT set_config('test.init', ((SELECT commit_hash FROM dolt_log WHERE message = 'Initialize data repository'))::text, FALSE);",
                "SELECT dolt_branch('b', current_setting('test.init', TRUE));",
                "SELECT * FROM dolt_checkout('b');",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-Am', 'C');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('b', '--no-commit');",
                "SELECT dolt_commit('-Am', 'M');",
                "SELECT set_config('test.merge', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'N');",
                "INSERT INTO t VALUES (4);",
                "SELECT dolt_commit('-am', 'O');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'collapsed', '--first', current_setting('test.merge', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_squash_history", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');",
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
                    query: "SELECT count(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("collapsed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message IN ('M', 'N', 'O');",
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
                    query: "SELECT count(*) FROM dolt_log WHERE message IN ('A', 'C');",
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
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: merge inside the collapsed range succeeds",
            intent: "dolt_squash_history collapses a range that contains a merge commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'A');",
                "SELECT set_config('test.a', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT set_config('test.init', ((SELECT commit_hash FROM dolt_log WHERE message = 'Initialize data repository'))::text, FALSE);",
                "SELECT dolt_branch('b', current_setting('test.init', TRUE));",
                "SELECT * FROM dolt_checkout('b');",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-Am', 'C');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('b', '--no-commit');",
                "SELECT dolt_commit('-Am', 'M');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'N');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'flattened', '--first', current_setting('test.a', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_squash_history", TEXT)],
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
                            &[T("flattened")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
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
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: error when --message is omitted",
            intent: "dolt_squash_history refuses to run without --message.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'c1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "must provide a commit message with --message", ..E }),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: error when --first is the initial commit",
            intent: "dolt_squash_history refuses --first naming the initial commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT set_config('test.init', ((SELECT commit_hash FROM dolt_log WHERE message = 'Initialize data repository'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'x', '--first', current_setting('test.init', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--first cannot be the initial commit", ..E }),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: error with uncommitted changes",
            intent: "dolt_squash_history refuses while the working set has changes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot squash history with uncommitted changes", ..E }),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: aborts when a merge is in progress",
            intent: "dolt_squash_history refuses while a merge is in progress.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET v = 2 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'main change');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE t SET v = 3 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'other change');",
                "SELECT * FROM dolt_checkout('main');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot squash history while a merge is in progress; abort the merge first", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: aborts when a cherry-pick is in progress",
            intent: "dolt_squash_history refuses while a cherry-pick is in progress.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE t SET v = 2 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'feature change');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET v = 3 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'main change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(dolt_hashof('feature'));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot squash history while a merge is in progress; abort the merge first", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: aborts when a revert is in progress",
            intent: "dolt_squash_history refuses while a revert is in progress.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "UPDATE t SET v = 2 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'c2');",
                "SELECT set_config('test.c2', (dolt_hashof('HEAD'))::text, FALSE);",
                "UPDATE t SET v = 99 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'c3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert(current_setting('test.c2', TRUE));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot squash history while a merge is in progress; abort the merge first", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: aborts when a rebase is in progress",
            intent: "dolt_squash_history refuses while a rebase is in progress.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'c3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'HEAD~2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_status;",
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
                    query: "SELECT dolt_squash_history('--message', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot squash history while a rebase is in progress; abort the rebase first", ..E }),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: error when --first is not an ancestor of HEAD",
            intent: "dolt_squash_history refuses a --first that isn't an ancestor of HEAD.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'c2');",
                "SELECT * FROM dolt_checkout('-b', 'side');",
                "INSERT INTO t VALUES (50);",
                "SELECT dolt_commit('-am', 'sidecommit');",
                "SELECT set_config('test.side', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'main3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'x', '--first', current_setting('test.side', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--first must be an ancestor of HEAD", ..E }),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: error when the initial commit has multiple children and no --first",
            intent: "Without --first, dolt_squash_history refuses when the initial commit has several children.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'A');",
                "SELECT set_config('test.init', ((SELECT commit_hash FROM dolt_log WHERE message = 'Initialize data repository'))::text, FALSE);",
                "SELECT dolt_branch('b', current_setting('test.init', TRUE));",
                "SELECT * FROM dolt_checkout('b');",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-Am', 'C');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('b', '--no-commit');",
                "SELECT dolt_commit('-Am', 'M');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the initial commit has multiple children; specify --first with the commit to squash from", ..E }),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: default resolves when only one child of the initial commit is reachable",
            intent: "Without --first, dolt_squash_history uses the one child of the initial commit HEAD can reach.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'A');",
                "SELECT set_config('test.init', ((SELECT commit_hash FROM dolt_log WHERE message = 'Initialize data repository'))::text, FALSE);",
                "SELECT dolt_branch('other', current_setting('test.init', TRUE));",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (99);",
                "SELECT dolt_commit('-Am', 'C');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'B');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'D');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'sq');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_squash_history", TEXT)],
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
                            &[T("sq")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log AS OF 'other';",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("C")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
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
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
        ScriptTest {
            name: "dolt_squash_history: error when the branch has only the initial commit",
            intent: "dolt_squash_history refuses a branch with only the initial commit.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_squash_history('--message', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "nothing to squash: the branch contains only the initial commit", ..E }),
                    ..A
                },
            ],
            skip: Some("2f core: dolt_squash_history is not supported yet"),
            ..S
        },
    ]);
}
