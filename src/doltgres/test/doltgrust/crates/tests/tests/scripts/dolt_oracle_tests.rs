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

//! The `tests` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_tests() {
    run_scripts(&[
        ScriptTest {
            name: r#"fresh_select_empty"#,
            intent: r#"Doltlite oracle case: fresh select empty."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_tests;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"test_name"#, TEXT), Column(r#"test_group"#, TEXT), Column(r#"test_query"#, TEXT), Column(r#"assertion_type"#, TEXT), Column(r#"assertion_comparator"#, TEXT), Column(r#"assertion_value"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T|', test_name, '|', coalesce((test_group)::text, ('')::text), '|', test_query, '|', assertion_type, '|', assertion_comparator, '|', coalesce((assertion_value)::text, ('NULL')::text)) FROM dolt_tests;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"fresh_read_no_status"#,
            intent: r#"Doltlite oracle case: fresh read no status."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_tests;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"test_name"#, TEXT), Column(r#"test_group"#, TEXT), Column(r#"test_query"#, TEXT), Column(r#"assertion_type"#, TEXT), Column(r#"assertion_comparator"#, TEXT), Column(r#"assertion_value"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S|', table_name, '|', staged, '|', status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"insert_materializes"#,
            intent: r#"Doltlite oracle case: insert materializes."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('row_count', 'basic', 'SELECT 1', 'expected_rows', '==', '1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T|', test_name, '|', coalesce((test_group)::text, ('')::text), '|', test_query, '|', assertion_type, '|', assertion_comparator, '|', coalesce((assertion_value)::text, ('NULL')::text)) FROM dolt_tests;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|row_count|basic|SELECT 1|expected_rows|==|1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"zero_row_delete_materializes"#,
            intent: r#"Doltlite oracle case: zero row delete materializes."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_tests WHERE test_name='missing';"#,
                    expected: Expected::Tag("DELETE 0"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S|', table_name, '|', staged, '|', status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::Differs(r#"dolt_tests is an ordinary table every database has, so deleting no rows from it changes nothing"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rollback_first_write"#,
            intent: r#"Doltlite oracle case: rollback first write."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('rolled',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T|', (SELECT count(*) FROM dolt_tests), '|',
      (SELECT count(*) FROM dolt_status WHERE table_name='public.dolt_tests'));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|0|0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rollback_after_zero_row_materialization"#,
            intent: r#"Doltlite oracle case: rollback after zero row materialization."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_tests WHERE test_name='missing';"#,
                    expected: Expected::Tag("DELETE 0"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('rolled',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T|', (SELECT count(*) FROM dolt_tests), '|',
      (SELECT count(*) FROM dolt_status WHERE table_name='public.dolt_tests'));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|0|0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::Differs(r#"dolt_tests is an ordinary table every database has, so deleting no rows from it changes nothing"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"update_replace_delete"#,
            intent: r#"Doltlite oracle case: update replace delete."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('one', 'g', 'SELECT 1', 'expected_rows', '==', '1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('two', NULL, 'SELECT 2', 'expected_single_value', '==', '2');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_tests SET test_group='updated' WHERE test_name='one';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('two', 'replaced', 'SELECT 3', 'expected_single_value', '==', '3') ON CONFLICT ("test_name") DO UPDATE SET "test_group" = EXCLUDED."test_group", "test_query" = EXCLUDED."test_query", "assertion_type" = EXCLUDED."assertion_type", "assertion_comparator" = EXCLUDED."assertion_comparator", "assertion_value" = EXCLUDED."assertion_value";"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_tests WHERE test_name='one';"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T|', test_name, '|', coalesce((test_group)::text, ('')::text), '|', test_query, '|', assertion_type, '|', assertion_comparator, '|', coalesce((assertion_value)::text, ('NULL')::text)) FROM dolt_tests;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|two|replaced|SELECT 3|expected_single_value|==|3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"nullable_group_value"#,
            intent: r#"Doltlite oracle case: nullable group value."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests(test_name,test_query,assertion_type,assertion_comparator)
VALUES ('null_value', 'SELECT NULL', 'expected_single_value', '==');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T|', test_name, '|', coalesce((test_group)::text, ('')::text), '|', test_query, '|', assertion_type, '|', assertion_comparator, '|', coalesce((assertion_value)::text, ('NULL')::text)) FROM dolt_tests;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|null_value||SELECT NULL|expected_single_value|==|NULL"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"duplicate_pk_err"#,
            intent: r#"Doltlite oracle case: duplicate pk err."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('dup',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('dup',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "dolt_tests_pkey""#, detail: r#"Key (test_name)=(dup) already exists."#, schema: r#"pg_catalog"#, table: r#"dolt_tests"#, constraint: r#"dolt_tests_pkey"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"invalid_assertion_err"#,
            intent: r#"Doltlite oracle case: invalid assertion err."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('bad',NULL,'SELECT 1','row_count','==','1');"#,
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "dolt_tests" violates check constraint "dolt_tests_assertion_type_check""#, detail: r#"Failing row contains (bad, null, SELECT 1, row_count, ==, 1)."#, schema: r#"pg_catalog"#, table: r#"dolt_tests"#, constraint: r#"dolt_tests_assertion_type_check"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"invalid_comparator_err"#,
            intent: r#"Doltlite oracle case: invalid comparator err."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('bad',NULL,'SELECT 1','expected_rows','=','1');"#,
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "dolt_tests" violates check constraint "dolt_tests_assertion_comparator_check""#, detail: r#"Failing row contains (bad, null, SELECT 1, expected_rows, =, 1)."#, schema: r#"pg_catalog"#, table: r#"dolt_tests"#, constraint: r#"dolt_tests_assertion_comparator_check"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"clean_after_commit"#,
            intent: r#"Doltlite oracle case: clean after commit."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('committed',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','add test');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S|', table_name, '|', staged, '|', status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"modified_after_commit"#,
            intent: r#"Doltlite oracle case: modified after commit."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('committed',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','add test');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_tests SET assertion_value='2';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S|', table_name, '|', staged, '|', status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S|dolt_tests|f|modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"branch_isolation"#,
            intent: r#"Doltlite oracle case: branch isolation."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('main_test',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b','feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('feature_test',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T|', test_name, '|', coalesce((test_group)::text, ('')::text), '|', test_query, '|', assertion_type, '|', assertion_comparator, '|', coalesce((assertion_value)::text, ('NULL')::text)) FROM dolt_tests;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|main_test||SELECT 1|expected_rows|==|1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"clean_merge"#,
            intent: r#"Doltlite oracle case: clean merge."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('base',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b','feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('feature',NULL,'SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"fast_forward"#, INT8), Column(r#"conflicts"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T|', test_name, '|', coalesce((test_group)::text, ('')::text), '|', test_query, '|', assertion_type, '|', assertion_comparator, '|', coalesce((assertion_value)::text, ('NULL')::text)) FROM dolt_tests;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|base||SELECT 1|expected_rows|==|1"#)],
                            &[T(r#"T|feature||SELECT 1|expected_rows|==|1"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"no_args_runs_all"#,
            intent: r#"Doltlite oracle case: no args runs all."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE fixture(i int PRIMARY KEY, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO fixture VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('a_rows_pass','group_a','SELECT i FROM fixture','expected_rows','==','2'),
 ('b_rows_fail','group_a','SELECT i FROM fixture','expected_rows','==','3'),
 ('c_cols_pass','group_b','SELECT i,s FROM fixture LIMIT 1','expected_columns','==','2'),
 ('d_value_pass','group_b','SELECT count(*) FROM fixture','expected_single_value','>=','2'),
 ('e_text_pass',NULL,'SELECT s FROM fixture WHERE i=1','expected_single_value','==','one');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run();"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|a_rows_pass|group_a|SELECT i FROM fixture|PASS|"#)],
                            &[T(r#"R|b_rows_fail|group_a|SELECT i FROM fixture|FAIL|Assertion failed: expected_rows equal to 3, got 2"#)],
                            &[T(r#"R|c_cols_pass|group_b|SELECT i,s FROM fixture LIMIT 1|PASS|"#)],
                            &[T(r#"R|d_value_pass|group_b|SELECT count(*) FROM fixture|PASS|"#)],
                            &[T(r#"R|e_text_pass||SELECT s FROM fixture WHERE i=1|PASS|"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"wildcard_runs_all"#,
            intent: r#"Doltlite oracle case: wildcard runs all."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE fixture(i int PRIMARY KEY, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO fixture VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('a_rows_pass','group_a','SELECT i FROM fixture','expected_rows','==','2'),
 ('b_rows_fail','group_a','SELECT i FROM fixture','expected_rows','==','3'),
 ('c_cols_pass','group_b','SELECT i,s FROM fixture LIMIT 1','expected_columns','==','2'),
 ('d_value_pass','group_b','SELECT count(*) FROM fixture','expected_single_value','>=','2'),
 ('e_text_pass',NULL,'SELECT s FROM fixture WHERE i=1','expected_single_value','==','one');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('*');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|a_rows_pass|group_a|SELECT i FROM fixture|PASS|"#)],
                            &[T(r#"R|b_rows_fail|group_a|SELECT i FROM fixture|FAIL|Assertion failed: expected_rows equal to 3, got 2"#)],
                            &[T(r#"R|c_cols_pass|group_b|SELECT i,s FROM fixture LIMIT 1|PASS|"#)],
                            &[T(r#"R|d_value_pass|group_b|SELECT count(*) FROM fixture|PASS|"#)],
                            &[T(r#"R|e_text_pass||SELECT s FROM fixture WHERE i=1|PASS|"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"individual_name"#,
            intent: r#"Doltlite oracle case: individual name."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE fixture(i int PRIMARY KEY, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO fixture VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('a_rows_pass','group_a','SELECT i FROM fixture','expected_rows','==','2'),
 ('b_rows_fail','group_a','SELECT i FROM fixture','expected_rows','==','3'),
 ('c_cols_pass','group_b','SELECT i,s FROM fixture LIMIT 1','expected_columns','==','2'),
 ('d_value_pass','group_b','SELECT count(*) FROM fixture','expected_single_value','>=','2'),
 ('e_text_pass',NULL,'SELECT s FROM fixture WHERE i=1','expected_single_value','==','one');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('e_text_pass');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|e_text_pass||SELECT s FROM fixture WHERE i=1|PASS|"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"group_name"#,
            intent: r#"Doltlite oracle case: group name."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE fixture(i int PRIMARY KEY, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO fixture VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('a_rows_pass','group_a','SELECT i FROM fixture','expected_rows','==','2'),
 ('b_rows_fail','group_a','SELECT i FROM fixture','expected_rows','==','3'),
 ('c_cols_pass','group_b','SELECT i,s FROM fixture LIMIT 1','expected_columns','==','2'),
 ('d_value_pass','group_b','SELECT count(*) FROM fixture','expected_single_value','>=','2'),
 ('e_text_pass',NULL,'SELECT s FROM fixture WHERE i=1','expected_single_value','==','one');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('group_b');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|c_cols_pass|group_b|SELECT i,s FROM fixture LIMIT 1|PASS|"#)],
                            &[T(r#"R|d_value_pass|group_b|SELECT count(*) FROM fixture|PASS|"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"multiple_arguments"#,
            intent: r#"Doltlite oracle case: multiple arguments."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE fixture(i int PRIMARY KEY, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO fixture VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('a_rows_pass','group_a','SELECT i FROM fixture','expected_rows','==','2'),
 ('b_rows_fail','group_a','SELECT i FROM fixture','expected_rows','==','3'),
 ('c_cols_pass','group_b','SELECT i,s FROM fixture LIMIT 1','expected_columns','==','2'),
 ('d_value_pass','group_b','SELECT count(*) FROM fixture','expected_single_value','>=','2'),
 ('e_text_pass',NULL,'SELECT s FROM fixture WHERE i=1','expected_single_value','==','one');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('e_text_pass','group_b');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|e_text_pass||SELECT s FROM fixture WHERE i=1|PASS|"#)],
                            &[T(r#"R|c_cols_pass|group_b|SELECT i,s FROM fixture LIMIT 1|PASS|"#)],
                            &[T(r#"R|d_value_pass|group_b|SELECT count(*) FROM fixture|PASS|"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"all_comparators"#,
            intent: r#"Doltlite oracle case: all comparators."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('eq','cmp','SELECT 2','expected_single_value','==','2'),
 ('ne','cmp','SELECT 2','expected_single_value','!=','3'),
 ('lt','cmp','SELECT 2','expected_single_value','<','3'),
 ('le','cmp','SELECT 2','expected_single_value','<=','2'),
 ('gt','cmp','SELECT 2','expected_single_value','>','1'),
 ('ge','cmp','SELECT 2','expected_single_value','>=','2');"#,
                    expected: Expected::Tag("INSERT 0 6"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('cmp');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|eq|cmp|SELECT 2|PASS|"#)],
                            &[T(r#"R|ge|cmp|SELECT 2|PASS|"#)],
                            &[T(r#"R|gt|cmp|SELECT 2|PASS|"#)],
                            &[T(r#"R|le|cmp|SELECT 2|PASS|"#)],
                            &[T(r#"R|lt|cmp|SELECT 2|PASS|"#)],
                            &[T(r#"R|ne|cmp|SELECT 2|PASS|"#)],
                        ],
                        tag: "SELECT 6",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"null_values"#,
            intent: r#"Doltlite oracle case: null values."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests(test_name,test_group,test_query,assertion_type,assertion_comparator) VALUES
 ('null_eq','nulls','SELECT NULL','expected_single_value','=='),
 ('null_ne','nulls','SELECT NULL','expected_single_value','!=');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('nulls');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|null_eq|nulls|SELECT NULL|PASS|"#)],
                            &[T(r#"R|null_ne|nulls|SELECT NULL|FAIL|Assertion failed: expected_single_value not equal to NULL, got NULL"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"result_shape_errors"#,
            intent: r#"Doltlite oracle case: result shape errors."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE fixture(i int PRIMARY KEY, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO fixture VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('many_cols','shape','SELECT i,s FROM fixture LIMIT 1','expected_single_value','==','1'),
 ('many_rows','shape','SELECT i FROM fixture','expected_single_value','==','1'),
 ('zero_rows','shape','SELECT i FROM fixture WHERE 0','expected_single_value','==','1');"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('shape');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|many_cols|shape|SELECT i,s FROM fixture LIMIT 1|FAIL|expected_single_value expects exactly one cell. Received multiple columns"#)],
                            &[T(r#"R|many_rows|shape|SELECT i FROM fixture|FAIL|expected_single_value expects exactly one cell. Received multiple rows"#)],
                            &[T(r#"R|zero_rows|shape|SELECT i FROM fixture WHERE 0|FAIL|query error: argument of WHERE must be type boolean, not type integer"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"The test's query uses MySQL's integer WHERE condition, which Postgres rejects since WHERE needs a boolean"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"query_validation"#,
            intent: r#"Doltlite oracle case: query validation."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('multi','validation','SELECT 1; SELECT 2','expected_rows','==','1'),
 ('recursive','validation','SELECT * FROM dolt_test_run()','expected_rows','==','1'),
 ('write','validation','CREATE TABLE nope(x INT)','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('validation');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|multi|validation|SELECT 1; SELECT 2|FAIL|Can only run exactly one query"#)],
                            &[T(r#"R|recursive|validation|SELECT * FROM dolt_test_run()|FAIL|Cannot call dolt_test_run in dolt_tests"#)],
                            &[T(r#"R|write|validation|CREATE TABLE nope(x INT)|FAIL|Cannot execute write queries"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"attach_rejected"#,
            intent: r#"Doltlite oracle case: attach rejected."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('attach',NULL,'ATTACH '':memory:'' AS aux','expected_rows','==','0');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', status) FROM dolt_test_run('attach');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|attach|FAIL"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"non_integer_counts"#,
            intent: r#"Doltlite oracle case: non integer counts."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('bad_cols','bad_int','SELECT 1','expected_columns','==','0.5'),
 ('bad_rows','bad_int','SELECT 1','expected_rows','==','0.5');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('bad_int');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|bad_cols|bad_int|SELECT 1|FAIL|cannot run assertion on non integer value: 0.5"#)],
                            &[T(r#"R|bad_rows|bad_int|SELECT 1|FAIL|cannot run assertion on non integer value: 0.5"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"scalar_types"#,
            intent: r#"Doltlite oracle case: scalar types."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE typed_values(d DATE, f double precision, b BOOLEAN, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO typed_values VALUES ('2025-08-22',3.14159,true,'String');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('boolean','types','SELECT b FROM typed_values','expected_single_value','==','true'),
 ('date','types','SELECT d FROM typed_values','expected_single_value','<','2025-08-23'),
 ('float','types','SELECT f FROM typed_values','expected_single_value','<','3.2'),
 ('string','types','SELECT s FROM typed_values','expected_single_value','==','String');"#,
                    expected: Expected::Tag("INSERT 0 4"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('types');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|boolean|types|SELECT b FROM typed_values|PASS|"#)],
                            &[T(r#"R|date|types|SELECT d FROM typed_values|PASS|"#)],
                            &[T(r#"R|float|types|SELECT f FROM typed_values|PASS|"#)],
                            &[T(r#"R|string|types|SELECT s FROM typed_values|PASS|"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"scalar_type_failures"#,
            intent: r#"Doltlite oracle case: scalar type failures."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE typed_failures(
 d DATE, dt timestamp, f double precision, amount DECIMAL(10,2), b BOOLEAN, s TEXT
);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO typed_failures VALUES
 ('2025-08-22','2025-08-22 09:00:00',3.14159,10.4,true,'String');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('bool_fail','type_failures','SELECT b FROM typed_failures','expected_single_value','==','false'),
 ('date_fail','type_failures','SELECT d FROM typed_failures','expected_single_value','>','2025-08-23'),
 ('decimal_fail','type_failures','SELECT amount FROM typed_failures','expected_single_value','>','10.5'),
 ('datetime_fail','type_failures','SELECT dt FROM typed_failures','expected_single_value','>','2025-08-22 09:00:01'),
 ('float_fail','type_failures','SELECT f FROM typed_failures','expected_single_value','>','3.2'),
 ('string_fail','type_failures','SELECT s FROM typed_failures','expected_single_value','!=','String');"#,
                    expected: Expected::Tag("INSERT 0 6"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('type_failures');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|bool_fail|type_failures|SELECT b FROM typed_failures|FAIL|Assertion failed: expected_single_value equal to false, got true"#)],
                            &[T(r#"R|date_fail|type_failures|SELECT d FROM typed_failures|FAIL|Assertion failed: expected_single_value greater than 2025-08-23, got 2025-08-22"#)],
                            &[T(r#"R|decimal_fail|type_failures|SELECT amount FROM typed_failures|FAIL|Assertion failed: expected_single_value greater than 10.5, got 10.40"#)],
                            &[T(r#"R|float_fail|type_failures|SELECT f FROM typed_failures|FAIL|Assertion failed: expected_single_value greater than 3.2, got 3.14159"#)],
                            &[T(r#"R|string_fail|type_failures|SELECT s FROM typed_failures|FAIL|Assertion failed: expected_single_value not equal to String, got String"#)],
                            &[Any],
                        ],
                        tag: "SELECT 6",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"zero_row_columns"#,
            intent: r#"Doltlite oracle case: zero row columns."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('zero_cols','zero','SELECT 1,2 WHERE 0','expected_columns','==','0');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('zero');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|zero_cols|zero|SELECT 1,2 WHERE 0|FAIL|query error: argument of WHERE must be type boolean, not type integer"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"The test's query uses MySQL's integer WHERE condition, which Postgres rejects since WHERE needs a boolean"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"delimiter_optional"#,
            intent: r#"Doltlite oracle case: delimiter optional."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('no_semicolon','delimiter','SELECT 1','expected_rows','==','1'),
 ('semicolon','delimiter','SELECT 1;','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('delimiter');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|no_semicolon|delimiter|SELECT 1|PASS|"#)],
                            &[T(r#"R|semicolon|delimiter|SELECT 1;|PASS|"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"name_precedes_group"#,
            intent: r#"Doltlite oracle case: name precedes group."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('chosen','other','SELECT 1','expected_rows','==','1'),
 ('group_member','chosen','SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('chosen');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|chosen|other|SELECT 1|PASS|"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"duplicate_arguments_repeat_results"#,
            intent: r#"Doltlite oracle case: duplicate arguments repeat results."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('repeat','repeats','SELECT 1','expected_rows','==','1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('repeat','repeat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|repeat|repeats|SELECT 1|PASS|"#)],
                            &[T(r#"R|repeat|repeats|SELECT 1|PASS|"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"query_error"#,
            intent: r#"Doltlite oracle case: query error."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('missing_table','errors','SELECT * FROM absent','expected_rows','==','0');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('R|', test_name, '|', test_group_name, '|', query, '|', status, '|', message) FROM dolt_test_run('errors');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|missing_table|errors|SELECT * FROM absent|FAIL|query error: relation "absent" does not exist"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"The failure message carries Postgres' error for the missing relation, where Dolt's carries MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"missing_argument_err"#,
            intent: r#"Doltlite oracle case: missing argument err."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE fixture(i int PRIMARY KEY, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO fixture VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('a_rows_pass','group_a','SELECT i FROM fixture','expected_rows','==','2'),
 ('b_rows_fail','group_a','SELECT i FROM fixture','expected_rows','==','3'),
 ('c_cols_pass','group_b','SELECT i,s FROM fixture LIMIT 1','expected_columns','==','2'),
 ('d_value_pass','group_b','SELECT count(*) FROM fixture','expected_single_value','>=','2'),
 ('e_text_pass',NULL,'SELECT s FROM fixture WHERE i=1','expected_single_value','==','one');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_test_run('missing');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"could not find tests for argument: missing"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"nonliteral_argument_err"#,
            intent: r#"Doltlite oracle case: nonliteral argument err."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE fixture(i int PRIMARY KEY, s TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO fixture VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
 ('a_rows_pass','group_a','SELECT i FROM fixture','expected_rows','==','2'),
 ('b_rows_fail','group_a','SELECT i FROM fixture','expected_rows','==','3'),
 ('c_cols_pass','group_b','SELECT i,s FROM fixture LIMIT 1','expected_columns','==','2'),
 ('d_value_pass','group_b','SELECT count(*) FROM fixture','expected_single_value','>=','2'),
 ('e_text_pass',NULL,'SELECT s FROM fixture WHERE i=1','expected_single_value','==','one');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_test_run(upper('group_a'));"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"could not find tests for argument: GROUP_A"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"no_tests_wildcard_err"#,
            intent: r#"Doltlite oracle case: no tests wildcard err."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_test_run();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"could not find tests for argument: *"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
    ]);
}
