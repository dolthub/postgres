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

//! Dolt's integration-tests/bats/1pk5col-strings.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_1pk5col_strings() {
    run_scripts(&[
        ScriptTest {
            name: r#"1pk5col-strings: dolt sql with string comparison operators"#,
            intent: r#"String keys compare with =, >, >=, and <> in their collation's order."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
                r#"INSERT INTO test VALUES ('tim', 'is', 'super', 'duper', 'rad', 'fo sho');"#,
                r#"INSERT INTO test VALUES ('zach', 'is', 'super', 'duper', 'not', 'rad');"#,
                r#"INSERT INTO test VALUES ('this', 'test', 'is', 'a', 'good', 'test');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
                        rows: &[
                            &[T("tim"), T("is"), T("super"), T("duper"), T("rad"), T("fo sho")],
                            &[T("zach"), T("is"), T("super"), T("duper"), T("not"), T("rad")],
                            &[T("this"), T("test"), T("is"), T("a"), T("good"), T("test")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE pk='tim';"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
                        rows: &[
                            &[T("tim"), T("is"), T("super"), T("duper"), T("rad"), T("fo sho")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE pk>'tim';"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
                        rows: &[
                            &[T("zach"), T("is"), T("super"), T("duper"), T("not"), T("rad")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE pk>='tim';"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
                        rows: &[
                            &[T("tim"), T("is"), T("super"), T("duper"), T("rad"), T("fo sho")],
                            &[T("zach"), T("is"), T("super"), T("duper"), T("not"), T("rad")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE pk<>'tim';"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
                        rows: &[
                            &[T("zach"), T("is"), T("super"), T("duper"), T("not"), T("rad")],
                            &[T("this"), T("test"), T("is"), T("a"), T("good"), T("test")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE pk='bob';"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
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
            name: r#"1pk5col-strings: interact with a strings type table with sql"#,
            intent: r#"A row of strings inserts and reads back whole or by chosen columns."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES ('tim','is','super','duper','rad','fo sho');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
                        rows: &[
                            &[T("tim"), T("is"), T("super"), T("duper"), T("rad"), T("fo sho")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk,c1,c4 FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c4", TEXT)],
                        rows: &[
                            &[T("tim"), T("is"), T("rad")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"1pk5col-strings: insert must use quoted strings"#,
            intent: r#"String values must be quoted."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk,c1,c2,c3,c4,c5) VALUES (tim,is,super,duper,rad,'fo sho');"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near ",""#, position: 52, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's parse error is Postgres' syntax error, at the comma after the keyword is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"1pk5col-strings: create and view a table with NULL and empty string values"#,
            intent: r#"Columns left out of an insert are NULL, which differs from an empty string."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
                r#"INSERT INTO test VALUES ('tim', '', '', '', '', '');"#,
                r#"INSERT INTO test (pk) VALUES ('aaron');"#,
                r#"INSERT INTO test (pk) VALUES ('brian');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
                        rows: &[
                            &[T("aaron"), Null, Null, Null, Null, Null],
                            &[T("brian"), Null, Null, Null, Null, Null],
                            &[T("tim"), T(""), T(""), T(""), T(""), T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"1pk5col-strings: semicolons in quoted sql statements"#,
            intent: r#"Semicolons and doubled quotes inside a quoted string are part of the string."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk)); COMMENT ON COLUMN test.pk IS 'tag:0'; COMMENT ON COLUMN test.c1 IS 'tag:1'; COMMENT ON COLUMN test.c2 IS 'tag:2'; COMMENT ON COLUMN test.c3 IS 'tag:3'; COMMENT ON COLUMN test.c4 IS 'tag:4'; COMMENT ON COLUMN test.c5 IS 'tag:5';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk,c1) VALUES ('test', 'this; should; work');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk,c1) VALUES ('test2', 'this; should; work');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk,c1) VALUES ('test3', 'this \\'' should \\'' work');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("Postgres' standard strings keep a backslash as it is, where MySQL reads \\\\ as one backslash")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c1 FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT)],
                        rows: &[
                            &[T("test"), T("this; should; work")],
                            &[T("test2"), T("this; should; work")],
                            &[T("test3"), T("this \\\\' should \\\\' work")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
