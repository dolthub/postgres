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

//! dolt_gc() and dolt_reflog(), with expectations from Dolt main 614771d742 (out/dolt-gc-oracle's cases).

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_reflog() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_gc empties the reflog and collects the commits only it named",
            intent: "dolt_gc empties the reflog, which lists ref moves newest first, and a commit only the reflog named is gone after it.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ref, commit_message FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), T("first")],
                            &[T("refs/heads/main"), T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
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
                    query: "SELECT ref, commit_message FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (9, 9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-am', 'after gc')) > 0;",
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
                    query: "SELECT ref, commit_message FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), T("after gc")],
                            &[T("refs/heads/main"), T("first")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'side');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'side'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.side', dolt_commit('-am', 'side commit'), FALSE) IS NOT NULL;",
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
                    query: "SELECT dolt_branch('-D', 'side');",
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
                    query: "SELECT ref, commit_message, commit_hash = current_setting('test.side', TRUE) AS is_side FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT), Column("is_side", BOOL)],
                        rows: &[
                            &[T("refs/heads/side"), T("side commit"), T("t")],
                            &[T("refs/heads/side"), T("after gc"), T("f")],
                            &[T("refs/heads/main"), T("after gc"), T("f")],
                            &[T("refs/heads/main"), T("first"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log(current_setting('test.side', TRUE));",
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
                    query: "SELECT * FROM t AS OF current_setting('test.side', TRUE) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout(current_setting('test.side', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "dolt does not support a detached head state. To create a branch at this commit instead, run:", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout(current_setting('test.side', TRUE), '-b', 'revived');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'revived'")],
                        ],
                        tag: "SELECT 1",
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
                    query: "SELECT dolt_branch('-D', 'revived');",
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
                    query: "SELECT ref, commit_message FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log(current_setting('test.side', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF current_setting('test.side', TRUE) ORDER BY id;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout(current_setting('test.side', TRUE), '-b', 'revived');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: Unexpected error creating branch 'revived' : target commit not found", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_gc in an open transaction",
            intent: "dolt_gc inside an open transaction leaves the transaction's writes to commit.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_gc and dolt_reflog arguments",
            intent: "dolt_gc takes only its options, and dolt_reflog takes one ref, matched by its full name or its last part without case, and --all once.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_gc('extra');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: gc does not take positional arguments, but found 1: extra", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc('a', 'b');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: gc does not take positional arguments, but found 2: a, b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc('--bogus');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: unknown option `bogus'", ..E }),
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
                    query: "SELECT dolt_gc('--full');",
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
                    query: "SELECT length(dolt_commit('--allow-empty', '-m', 'second')) > 0;",
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
                    query: "SELECT * FROM dolt_reflog('--all', '--all');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: multiple values provided for `all`", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_reflog('a', 'b');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: dolt_reflog has too many positional arguments. Expected at most 1, found 2: ['a' 'b']", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_reflog('nosuch');",
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
                    query: "SELECT ref, commit_message FROM dolt_reflog('MAIN');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), T("second")],
                            &[T("refs/heads/main"), T("first")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref, commit_message FROM dolt_reflog('refs/heads/main', '--all');",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), T("second")],
                            &[T("refs/heads/main"), T("first")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reflog: disabled with DOLT_DISABLE_REFLOG",
            intent: "With the reflog turned off, dolt_reflog lists no moves made since.",
            set_up_script: &[
                "ALTER SYSTEM SET reflog = off;",
                "SELECT pg_reload_conf();",
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_commit('--allow-empty', '-m', 'test commit 1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ref, commit_message FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), T("Initialize data repository")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("the reflog setting stops recording once it is set, where Dolt's environment variable is set before the repository exists, so the database's creation is still listed")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reflog: enabled by default",
            intent: "The reflog lists the branch's moves by default.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ref, commit_message FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), T("initial commit")],
                            &[T("refs/heads/main"), T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reflog: set DOLT_REFLOG_RECORD_LIMIT",
            intent: "A limit on the reflog's entries keeps only the newest moves.",
            set_up_script: &[
                "ALTER SYSTEM SET reflog_max_entries = 2;",
                "SELECT pg_reload_conf();",
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_commit('--allow-empty', '-m', 'test commit 1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'test commit 2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ref, commit_message FROM dolt_reflog();",
                    expected: Expected::Rows {
                        columns: &[Column("ref", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("refs/heads/main"), T("test commit 2")],
                            &[T("refs/heads/main"), T("test commit 1")],
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
