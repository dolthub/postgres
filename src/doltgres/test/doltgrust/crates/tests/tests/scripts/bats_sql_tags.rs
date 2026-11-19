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

//! Dolt's integration-tests/bats/sql-tags.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_tags() {
    run_scripts(&[
        ScriptTest {
            name: "sql-tags: DOLT_TAG works with a explicit ref",
            intent: "dolt_tag tags the commit a ref names.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test');",
                "DELETE FROM test WHERE pk = 0;",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'made changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1', 'HEAD^');",
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
                    query: "SELECT tag_name, tag_hash = dolt_hashof('HEAD^') FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("?column?", BOOL)],
                        rows: &[
                            &[T("v1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt tag lists the tags, which dolt_tags holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-tags: DOLT_TAG works with implicit head ref",
            intent: "dolt_tag without a ref tags HEAD.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test');",
                "DELETE FROM test WHERE pk = 0;",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'made changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1');",
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
                    query: "SELECT tag_name, tag_hash = dolt_hashof('HEAD') FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("?column?", BOOL)],
                        rows: &[
                            &[T("v1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt tag lists the tags, which dolt_tags holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-tags: DOLT_TAG works with author arg defined",
            intent: "dolt_tag --author names the tagger.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test');",
                "DELETE FROM test WHERE pk = 0;",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'made changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1', '--author', 'Jane Doe <jane@doe.com>');",
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
                    query: "SELECT tag_name, tagger, email FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT), Column("tagger", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T("v1"), T("Jane Doe"), T("jane@doe.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt tag -v prints the tagger, which dolt_tags holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-tags: create tag v1.2.3",
            intent: "A tag name can hold dots.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test');",
                "DELETE FROM test WHERE pk = 0;",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'made changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1.2.3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt skips it because its storage (Noms) refuses '.' in dataset names; Doltgrust's refs have no such limit")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("v1.2.3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-tags: delete a tag",
            intent: "dolt_tag -d deletes a tag.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test');",
                "DELETE FROM test WHERE pk = 0;",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'made changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1');",
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
                    query: "SELECT dolt_tag('-d', 'v1');",
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
                    query: "SELECT tag_name FROM dolt_tags;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt tag lists the tags, which dolt_tags holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-tags: use a tag as ref for diff",
            intent: "A tag names a revision to diff from.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test');",
                "DELETE FROM test WHERE pk = 0;",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'made changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1', 'HEAD^');",
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
                    query: "SELECT dolt_diff_type, from_pk, to_pk FROM dolt_diff('v1', 'WORKING', 'test') ORDER BY dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("added"), Null, T("3")],
                            &[T("removed"), T("0"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the diff, which dolt_diff() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-tags: use a tag as a ref for merge",
            intent: "A tag names a revision to merge.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table test');",
                "DELETE FROM test WHERE pk = 0;",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'made changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v1', 'HEAD');",
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
                    query: "SELECT * FROM dolt_checkout('-b', 'other', 'HEAD^');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (8),(9);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'made changes');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('v1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
