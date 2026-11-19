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

//! Dolt's integration-tests/bats/json-new-fmt.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_json_new_fmt() {
    run_scripts(&[
        ScriptTest {
            name: r#"json-new-fmt: Create table with JSON column"#,
            intent: r#"A table can have a JSON column."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: query JSON values"#,
            intent: r#"JSON values read back after inserts, updates, and deletes."#,
            set_up_script: &[
                r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                r#"INSERT INTO js VALUES (1, '{"a":1}'), (2, '{"b":2}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": 1}")],
                            &[T("2"), T("{\"b\": 2}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE js SET js = '{"c":3}' WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": 1}")],
                            &[T("2"), T("{\"c\": 3}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM js WHERE pk = 2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": 1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: JSON value printing"#,
            intent: r#"JSON objects and nested arrays print as their values."#,
            set_up_script: &[
                r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                r#"INSERT INTO js VALUES (1, '{"a":1}'), (2, '{"b":2}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": 1}")],
                            &[T("2"), T("{\"b\": 2}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt sql's table, CSV, and JSON output formats are the CLI's, so the port checks the rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO js VALUES (3, '["abc", 123, 1.5, {"a": 123, "b":[456, "def"]}]');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js WHERE pk = 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("3"), T("[\"abc\", 123, 1.5, {\"a\": 123, \"b\": [456, \"def\"]}]")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt sql's table, CSV, and JSON output formats are the CLI's, so the port checks the rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: diff JSON values"#,
            intent: r#"A diff shows a changed, a deleted, and an added JSON value."#,
            set_up_script: &[
                r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                r#"INSERT INTO js VALUES (1, '{"a":1}'), (2, '{"b":2}');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'added JSON table');"#,
                r#"UPDATE js SET js = '{"a":11}' WHERE pk = 1;"#,
                r#"DELETE FROM js WHERE pk = 2;"#,
                r#"INSERT INTO js VALUES (3, '{"c":3}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_diff_type, from_pk, from_js, to_pk, to_js FROM dolt_diff('HEAD', 'WORKING', 'js') ORDER BY coalesce(to_pk, from_pk);"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("from_js", JSONB), Column("to_pk", INT4), Column("to_js", JSONB)],
                        rows: &[
                            &[T("modified"), T("1"), T("{\"a\": 1}"), T("1"), T("{\"a\": 11}")],
                            &[T("removed"), T("2"), T("{\"b\": 2}"), Null, Null],
                            &[T("added"), Null, Null, T("3"), T("{\"c\": 3}")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff() for the rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: merge JSON values"#,
            intent: r#"Changes to different rows' JSON values merge, and changes to the same row's conflict, and resolving with ours keeps ours."#,
            set_up_script: &[
                r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                r#"INSERT INTO js VALUES (1, '{"a":1}'), (2, '{"b":2}');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'added JSON table');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"SELECT dolt_branch('another');"#,
                r#"UPDATE js SET js = '{"a":11}' WHERE pk = 1;"#,
                r#"SELECT dolt_commit('-am', 'made changes on branch main');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"UPDATE js SET js = '{"b":22}' WHERE pk = 2;"#,
                r#"SELECT dolt_commit('-am', 'made changes on branch other');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT hash, fast_forward, conflicts FROM dolt_merge('other', '--no-commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, which dolt_merge() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": 11}")],
                            &[T("2"), T("{\"b\": 22}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'merged other into main');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_checkout('another');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE js SET js = '{"b":99}' WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'made changes on branch another');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, whose CONFLICT line is dolt_merge's conflicts column, and the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'js');"#,
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
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": 1}")],
                            &[T("2"), T("{\"b\": 99}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: merge JSON values with stored procedure"#,
            intent: r#"Changes to different rows' JSON values merge, and changes to the same row's conflict, and resolving with ours keeps ours."#,
            set_up_script: &[
                r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                r#"INSERT INTO js VALUES (1, '{"a":1}'), (2, '{"b":2}');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'added JSON table');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"SELECT dolt_branch('another');"#,
                r#"UPDATE js SET js = '{"a":11}' WHERE pk = 1;"#,
                r#"SELECT dolt_commit('-am', 'made changes on branch main');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"UPDATE js SET js = '{"b":22}' WHERE pk = 2;"#,
                r#"SELECT dolt_commit('-am', 'made changes on branch other');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT hash, fast_forward, conflicts FROM dolt_merge('other', '--no-commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, which dolt_merge() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": 11}")],
                            &[T("2"), T("{\"b\": 22}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'merged other into main');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_checkout('another');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE js SET js = '{"b":99}' WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'made changes on branch another');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, whose CONFLICT line is dolt_merge's conflicts column, and the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'js');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": 1}")],
                            &[T("2"), T("{\"b\": 99}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: insert value with special characters"#,
            intent: r#"HTML characters in a JSON string read back as they were written."#,
            set_up_script: &[
                r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                r#"INSERT INTO js VALUES (1, '{"a":"<>&"}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"a\": \"<>&\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt sql's table, CSV, and JSON output formats are the CLI's, so the port checks the rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: insert array with special characters"#,
            intent: r#"HTML characters in a JSON array's string read back as they were written."#,
            set_up_script: &[
                r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                r#"INSERT INTO js VALUES (1, '[{"a":"<>&"}]');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("[{\"a\": \"<>&\"}]")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt sql's table, CSV, and JSON output formats are the CLI's, so the port checks the rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: json casting behavior"#,
            intent: r#"Casting text to JSON drops its trailing whitespace and decodes escaped characters."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT CAST(E'6\n' AS jsonb);"#,
                    expected: Expected::Rows {
                        columns: &[Column("jsonb", JSONB)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CAST('{"key":"\u0068\u0069"}' AS jsonb);"#,
                    expected: Expected::Rows {
                        columns: &[Column("jsonb", JSONB)],
                        rows: &[
                            &[T("{\"key\": \"hi\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt skips this assertion as one it fails and MySQL passes, and Postgres passes it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: round-tripping behavior"#,
            intent: r#"JSON values read back without their trailing whitespace and with escaped characters decoded."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk int PRIMARY KEY, col1 jsonb);"#,
                r#"SELECT dolt_commit('-Am', 'add table');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, E'6\n');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, col1 FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", JSONB)],
                        rows: &[
                            &[T("1"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, '{"key":"\u0068\u0069"}');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, col1 FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", JSONB)],
                        rows: &[
                            &[T("2"), T("{\"key\": \"hi\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt skips this assertion as one it fails and MySQL passes, and Postgres passes it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json-new-fmt: should store json minimally escaped"#,
            intent: r#"A JSON value written with escaped characters and one written plainly are the same value, so the two branches don't differ."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk int PRIMARY KEY, col1 jsonb);"#,
                r#"SELECT dolt_commit('-Am', 'add table');"#,
                r#"SELECT dolt_branch('other');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, '{"key":"\u0068\u0069"}');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, col1 FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"key\": \"hi\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt skips this assertion as one it fails and MySQL passes, and Postgres passes it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'left');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_checkout('other');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, '{"key":"hi"}');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, col1 FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"key\": \"hi\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'right');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('main', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff main is a CLI command, so the port reads dolt_diff_summary()")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
