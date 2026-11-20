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

//! Dolt's integration-tests/bats/revert.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_revert() {
    run_scripts(&[
        ScriptTest {
            name: "revert: HEAD",
            intent: "dolt_revert of HEAD makes a commit that undoes HEAD's changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
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
            name: "revert: HEAD~1",
            intent: "dolt_revert of HEAD~1 undoes only that commit's changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
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
            name: "revert: HEAD & HEAD~1",
            intent: "dolt_revert of two commits undoes both.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD', 'HEAD~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
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
            name: "revert: has changes in the working set",
            intent: "dolt_revert refuses to run over uncommitted changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Your local changes would be overwritten by revert.\nhint: Please commit your changes before you revert.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: --abort with no revert in progress",
            intent: "dolt_revert --abort with no revert in progress fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no revert in progress", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: --continue with no revert in progress",
            intent: "dolt_revert --continue with no revert in progress fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no revert in progress", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: too far back",
            intent: "dolt_revert of an ancestor past the first commit fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~10');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: init commit",
            intent: "dolt_revert of the first commit, which has no parent, fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~4');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust reverts a commit without parents against an empty root, as Git does, where Dolt refuses it, and the first commit's root is empty, so its revert changes nothing (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: invalid hash",
            intent: "dolt_revert of a hash naming no commit fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: HEAD with --author parameter",
            intent: "dolt_revert --author records the given author on the revert commit.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD', '--author', 'john <johndoe@gmail.com>');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Revert \"Inserted 3\""), T("john"), T("johndoe@gmail.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: HEAD & HEAD~1 with --author parameter",
            intent: "dolt_revert --author of two commits records the given author on both revert commits.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD', 'HEAD~1', '--author', 'john <johndoe@gmail.com>');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Revert \"Inserted 2\""), T("john"), T("johndoe@gmail.com")],
                            &[T("Revert \"Inserted 3\""), T("john"), T("johndoe@gmail.com")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: SQL HEAD",
            intent: "dolt_revert of HEAD makes a commit that undoes HEAD's changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
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
            name: "revert: SQL HEAD~1",
            intent: "dolt_revert of HEAD~1 undoes only that commit's changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
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
            name: "revert: SQL HEAD & HEAD~1",
            intent: "dolt_revert of two commits undoes both.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD', 'HEAD~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
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
            name: "revert: SQL has changes in the working set",
            intent: "dolt_revert refuses to run over uncommitted changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Your local changes would be overwritten by revert.\nhint: Please commit your changes before you revert.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: SQL --abort with no revert in progress",
            intent: "dolt_revert --abort with no revert in progress fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no revert in progress", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: SQL --continue with no revert in progress",
            intent: "dolt_revert --continue with no revert in progress fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no revert in progress", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: SQL too far back",
            intent: "dolt_revert of an ancestor past the first commit fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~10');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: SQL revert init commit",
            intent: "dolt_revert of the first commit, which has no parent, fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~4');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust reverts a commit without parents against an empty root, as Git does, where Dolt refuses it, and the first commit's root is empty, so its revert changes nothing (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: SQL invalid hash",
            intent: "dolt_revert of a hash naming no commit fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: SQL HEAD with author",
            intent: "dolt_revert --author records the given author on the revert commit.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD', '--author', 'john doe <johndoe@gmail.com>');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Revert \"Inserted 3\""), T("john doe"), T("johndoe@gmail.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "revert: SQL HEAD & HEAD~1 with author",
            intent: "dolt_revert --author of two commits records the given author on both revert commits.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_commit('-Am', 'Created table');",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'Inserted 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD', 'HEAD~1', '--author', 'john doe <johndoe@gmail.com>');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Revert \"Inserted 2\""), T("john doe"), T("johndoe@gmail.com")],
                            &[T("Revert \"Inserted 3\""), T("john doe"), T("johndoe@gmail.com")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: conflicts"#,
            intent: r#"Reverting a commit whose row a later commit changed stops with a conflict."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: constraint violations"#,
            intent: r#"Reverting a commit that deleted a child row whose parent a later commit deleted stops with a foreign key violation."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"CREATE UNIQUE INDEX ON parent (v1);"#,
                r#"CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));"#,
                r#"INSERT INTO parent VALUES (10, 1), (20, 2);"#,
                r#"INSERT INTO child VALUES (1, 1), (2, 2);"#,
                r#"SELECT dolt_commit('-Am', 'MC1');"#,
                r#"DELETE FROM child WHERE pk = 2;"#,
                r#"SELECT dolt_commit('-am', 'MC2');"#,
                r#"DELETE FROM parent WHERE pk = 20;"#,
                r#"SELECT dolt_commit('-am', 'MC3');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: --abort after conflict restores state"#,
            intent: r#"Aborting a revert that stopped with a conflict removes the conflict and leaves a clean working set."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
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
            name: r#"revert: --continue after resolving conflict"#,
            intent: r#"After its conflict is resolved and staged, --continue commits the revert."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"Inserted 4\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: multi-commit stops at first conflict"#,
            intent: r#"Reverting two commits commits the first revert and stops at the second's conflict."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"UPDATE test SET v1 = 6 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4 (again)');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD', 'HEAD~2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"Updated 4 (again)\"")],
                            &[T("Updated 4 (again)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: --abort after multi-commit revert restores original HEAD"#,
            intent: r#"Aborting a series of reverts that stopped at a conflict moves HEAD back to the commit before the series."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"UPDATE test SET v1 = 6 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4 (again)');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD', 'HEAD~2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"Updated 4 (again)\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Updated 4 (again)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
            name: r#"revert: SQL conflicts, @@autocommit=1"#,
            intent: r#"With autocommit, a revert that leaves a conflict fails and rolls back."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on"#, ..E }),
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: SQL conflicts, @@autocommit=0"#,
            intent: r#"In a transaction, a revert that leaves a conflict returns its count."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    dolt: Some(Dolt::MySqlOnly("@@autocommit=0 is MySQL's, so the port opens a transaction")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: SQL constraint violations, @@autocommit=0"#,
            intent: r#"In a transaction, a revert that leaves a foreign key violation returns its count."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"CREATE UNIQUE INDEX ON parent (v1);"#,
                r#"CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));"#,
                r#"INSERT INTO parent VALUES (10, 1), (20, 2);"#,
                r#"INSERT INTO child VALUES (1, 1), (2, 2);"#,
                r#"SELECT dolt_commit('-Am', 'MC1');"#,
                r#"DELETE FROM child WHERE pk = 2;"#,
                r#"SELECT dolt_commit('-am', 'MC2');"#,
                r#"DELETE FROM parent WHERE pk = 20;"#,
                r#"SELECT dolt_commit('-am', 'MC3');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    dolt: Some(Dolt::MySqlOnly("@@autocommit=0 is MySQL's, so the port opens a transaction")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: SQL --abort and --continue workflow"#,
            intent: r#"A revert kept with its conflict by dolt_allow_commit_conflicts can be aborted, leaving no conflicts."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
            name: r#"revert: SQL --continue after resolving conflict"#,
            intent: r#"After its conflict is resolved and staged, dolt_revert('--continue') commits the revert."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"Inserted 4\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert: SQL --abort after multi-commit revert restores original HEAD"#,
            intent: r#"Aborting a series of reverts that stopped at a conflict moves HEAD back to the commit before the series."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"UPDATE test SET v1 = 6 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4 (again)');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD', 'HEAD~2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"Updated 4 (again)\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Updated 4 (again)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
            name: r#"revert: --continue called multiple times for multi-commit series"#,
            intent: r#"Each --continue commits the revert whose conflict was resolved and goes on to the next, stopping at its conflict, until the series is done."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"UPDATE test SET v1 = 10 WHERE pk = 1;"#,
                r#"SELECT dolt_commit('-am', 'change pk1');"#,
                r#"SELECT set_config('test.a', dolt_hashof('HEAD'), false);"#,
                r#"UPDATE test SET v1 = 20 WHERE pk = 2;"#,
                r#"SELECT dolt_commit('-am', 'change pk2');"#,
                r#"SELECT set_config('test.b', dolt_hashof('HEAD'), false);"#,
                r#"UPDATE test SET v1 = 30 WHERE pk = 3;"#,
                r#"SELECT dolt_commit('-am', 'change pk3');"#,
                r#"SELECT set_config('test.c', dolt_hashof('HEAD'), false);"#,
                r#"UPDATE test SET v1 = 21 WHERE pk = 2;"#,
                r#"UPDATE test SET v1 = 31 WHERE pk = 3;"#,
                r#"SELECT dolt_commit('-am', 'clobber pk2 and pk3');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert(current_setting('test.a'), current_setting('test.b'), current_setting('test.c'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1 FROM test WHERE pk = 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET v1 = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"change pk2\"")],
                            &[T("Revert \"change pk1\"")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET v1 = 31 WHERE pk = 3;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"change pk3\"")],
                            &[T("Revert \"change pk2\"")],
                            &[T("Revert \"change pk1\"")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
            name: r#"revert: SQL --continue called multiple times for multi-commit series"#,
            intent: r#"Each dolt_revert('--continue') commits the revert whose conflict was resolved and goes on to the next, stopping at its conflict, until the series is done."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"UPDATE test SET v1 = 10 WHERE pk = 1;"#,
                r#"SELECT dolt_commit('-am', 'change pk1');"#,
                r#"SELECT set_config('test.a', dolt_hashof('HEAD'), false);"#,
                r#"UPDATE test SET v1 = 20 WHERE pk = 2;"#,
                r#"SELECT dolt_commit('-am', 'change pk2');"#,
                r#"SELECT set_config('test.b', dolt_hashof('HEAD'), false);"#,
                r#"UPDATE test SET v1 = 30 WHERE pk = 3;"#,
                r#"SELECT dolt_commit('-am', 'change pk3');"#,
                r#"SELECT set_config('test.c', dolt_hashof('HEAD'), false);"#,
                r#"UPDATE test SET v1 = 21 WHERE pk = 2;"#,
                r#"UPDATE test SET v1 = 31 WHERE pk = 3;"#,
                r#"SELECT dolt_commit('-am', 'clobber pk2 and pk3');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert(current_setting('test.a'), current_setting('test.b'), current_setting('test.c'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1 FROM test WHERE pk = 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET v1 = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"change pk2\"")],
                            &[T("Revert \"change pk1\"")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET v1 = 31 WHERE pk = 3;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"change pk3\"")],
                            &[T("Revert \"change pk2\"")],
                            &[T("Revert \"change pk1\"")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
            name: r#"revert: --continue with ignored table in working set"#,
            intent: r#"--continue commits the revert while a table dolt_ignore ignores sits in the working set."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"SELECT dolt_commit('-Am', 'Created table');"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"INSERT INTO dolt_ignore VALUES ('ignored_*', TRUE);"#,
                r#"CREATE TABLE ignored_1 (id int PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'Add ignore pattern');"#,
                r#"INSERT INTO test VALUES (4, 4);"#,
                r#"SELECT dolt_commit('-am', 'Inserted 4');"#,
                r#"UPDATE test SET v1 = 5 WHERE pk = 4;"#,
                r#"SELECT dolt_commit('-am', 'Updated 4');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE ignored_2 (id int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt revert is a CLI command, whose \"Automatic revert failed\" error is dolt_revert's counts, and the CLI keeps a revert's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"Inserted 4\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log is a CLI command, so the port reads dolt_log")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
