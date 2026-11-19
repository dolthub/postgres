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

//! Dolt's integration-tests/bats/commit_tags.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_commit_tags() {
    run_scripts(&[
        ScriptTest {
            name: r#"commit_tags: create a tag with a explicit ref"#,
            intent: r#"dolt_tag names a commit given by a ref."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0), (1), (2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"DELETE FROM test WHERE pk = 0;"#,
                r#"INSERT INTO test VALUES (3);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'made changes');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1', 'HEAD^');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"v1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt tag is a CLI command, so the port reads dolt_tags"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: create a tag with implicit head ref"#,
            intent: r#"dolt_tag with no ref names HEAD."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0), (1), (2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"DELETE FROM test WHERE pk = 0;"#,
                r#"INSERT INTO test VALUES (3);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'made changes');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"v1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt tag is a CLI command, so the port reads dolt_tags"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: create a tag with author arg given"#,
            intent: r#"dolt_tag --author records the tagger."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0), (1), (2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"DELETE FROM test WHERE pk = 0;"#,
                r#"INSERT INTO test VALUES (3);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'made changes');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1', '--author', 'John Doe <john@doe.com>');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name, tagger, email FROM dolt_tags;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("tagger", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T(r#"v1"#), T(r#"John Doe"#), T(r#"john@doe.com"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: create tag v1.2.3"#,
            intent: r#"A tag name may hold dots."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0), (1), (2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"DELETE FROM test WHERE pk = 0;"#,
                r#"INSERT INTO test VALUES (3);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'made changes');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1.2.3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: delete a tag"#,
            intent: r#"dolt_tag -d deletes a tag."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0), (1), (2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"DELETE FROM test WHERE pk = 0;"#,
                r#"INSERT INTO test VALUES (3);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'made changes');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('-d', 'v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt tag is a CLI command, so the port reads dolt_tags"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: use a tag as ref for diff"#,
            intent: r#"A tag names its commit in a diff."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0), (1), (2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"DELETE FROM test WHERE pk = 0;"#,
                r#"INSERT INTO test VALUES (3);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'made changes');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1', 'HEAD^');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_diff_type, from_pk, to_pk FROM dolt_diff('v1', 'WORKING', 'test') ORDER BY coalesce(from_pk, to_pk);"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T(r#"removed"#), T(r#"0"#), Null],
                            &[T(r#"added"#), Null, T(r#"3"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt diff is a CLI command, so the port reads dolt_diff"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: use a tag as a ref for merge"#,
            intent: r#"A tag names its commit in a merge."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0), (1), (2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"DELETE FROM test WHERE pk = 0;"#,
                r#"INSERT INTO test VALUES (3);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'made changes');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('other', 'HEAD~');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (8), (9);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'made changes');"#,
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
                    query: r#"SELECT * FROM dolt_merge('v1', '-m', 'merge v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                            &[T(r#"3"#)],
                            &[T(r#"8"#)],
                            &[T(r#"9"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: push/pull tags to/from a remote"#,
            intent: r#"Tags reach a clone only once pushed by name, then through a pull, with their messages."#,
            set_up_script: &[
                r#"CREATE DATABASE repo;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'table test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (7), (8), (9);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'more rows');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1', 'HEAD^');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v2', 'HEAD', '-m', 'SAMO');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Everything up-to-date"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo_clone');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--no-edit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo_clone",
                    database: "repo_clone",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "repo_clone",
                    database: "repo_clone",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt tag is a CLI command, so the port reads dolt_tags"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'v2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--no-edit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo_clone",
                    database: "repo_clone",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"v1"#)],
                            &[T(r#"v2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo_clone",
                    database: "repo_clone",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt tag is a CLI command, so the port reads dolt_tags"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_tags WHERE tag_name = 'v2';"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"SAMO"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo_clone",
                    database: "repo_clone",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: prevent cloning tags"#,
            intent: r#"A clone can't check out a tag as its branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'table test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo",
                    database: "repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('--branch', 'v1', 'file://{NEWDIR:remote}', 'repo_clone');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"this operation is not supported while in a detached head state"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_tags: create a tag with semver string"#,
            intent: r#"Tag names may look like version numbers, with or without a leading v."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0), (1), (2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"DELETE FROM test WHERE pk = 0;"#,
                r#"INSERT INTO test VALUES (3);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'made changes');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1.0.0', 'HEAD^');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"v1.0.0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt tag is a CLI command, so the port reads dolt_tags"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('1.0.0', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"1.0.0"#)],
                            &[T(r#"v1.0.0"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt tag is a CLI command, so the port reads dolt_tags"#)),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
