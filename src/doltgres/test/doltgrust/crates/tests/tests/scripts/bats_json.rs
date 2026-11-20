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

//! Dolt's integration-tests/bats/json.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_json() {
    run_scripts(&[
        ScriptTest {
            name: r#"json: Create table with JSON column"#,
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
            name: r#"json: query JSON values"#,
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
            name: r#"json: JSON value printing"#,
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
            name: r#"json: JSON value printing HTML characters"#,
            intent: r#"HTML characters in JSON keys and values print unescaped."#,
            set_up_script: &[
                r#"CREATE TABLE js (pk int PRIMARY KEY, js jsonb);"#,
                r#"INSERT INTO js VALUES (1, '{"<>&":"<>&"}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM js ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("js", JSONB)],
                        rows: &[
                            &[T("1"), T("{\"<>&\": \"<>&\"}")],
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
            name: r#"json: diff JSON values"#,
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
            name: r#"json: merge JSON values"#,
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
            name: r#"json: merge JSON values with stored procedure"#,
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
            name: r#"json: insert value with special characters"#,
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
            name: r#"json: insert array with special characters"#,
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
            name: r#"json: insert large string value (> 1MB)"#,
            intent: r#"A JSON document of more than 3 MB stores and reads back whole."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk int PRIMARY KEY, j1 jsonb);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, jsonb_build_object('large_value', repeat('x', 3145752)));"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt sql -f runs a file of MySQL statements, so the port builds a value of the same length with jsonb_build_object")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, length(j1::text) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("1"), T("3145771")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"json: JSON_SET with quoted field names"#,
            intent: r#"Setting JSON keys by paths that quote them, needed or not, and nested, sets the keys."#,
            set_up_script: &[
                r#"CREATE TABLE test_data (data jsonb);"#,
                r#"INSERT INTO test_data VALUES ('{}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"UPDATE test_data SET data = jsonb_set(data, '{a}', '"b"');"#,
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("JSON_SET and its $. paths are MySQL's, so the port uses jsonb_set with Postgres' path arrays, quoting the same keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT data FROM test_data;"#,
                    expected: Expected::Rows {
                        columns: &[Column("data", JSONB)],
                        rows: &[
                            &[T("{\"a\": \"b\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test_data SET data = jsonb_set(data, '{"a key"}', '"b"');"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT data FROM test_data;"#,
                    expected: Expected::Rows {
                        columns: &[Column("data", JSONB)],
                        rows: &[
                            &[T("{\"a\": \"b\", \"a key\": \"b\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test_data SET data = jsonb_set(data, '{"c"}', '"test"');"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT data FROM test_data;"#,
                    expected: Expected::Rows {
                        columns: &[Column("data", JSONB)],
                        rows: &[
                            &[T("{\"a\": \"b\", \"c\": \"test\", \"a key\": \"b\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test_data SET data = jsonb_set(data, '{"d"}', '{}');"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test_data SET data = jsonb_set(data, '{"d","e"}', '"nested"');"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT data FROM test_data;"#,
                    expected: Expected::Rows {
                        columns: &[Column("data", JSONB)],
                        rows: &[
                            &[T("{\"a\": \"b\", \"c\": \"test\", \"d\": {\"e\": \"nested\"}, \"a key\": \"b\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("MySQL's JSON is a normalized binary form like Postgres' jsonb, which prints a space after each colon and comma where Dolt prints none")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
