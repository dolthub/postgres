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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_test_table.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_test_table_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "can insert into dolt tests",
            intent: "A test can be inserted into dolt_tests.",
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('no tables', 'validate tables' , 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '0');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can drop dolt tests table, cannot drop twice",
            intent: "dolt_tests can be dropped once, and a second drop fails.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('no tables', 'validate tables', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE dolt_tests;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE dolt_tests;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::Differs("a versioned system table's definition is never stored, so DROP TABLE empties it and a second DROP empties it again, where Dolt's second DROP finds no table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can call delete from on dolt tests table",
            intent: "DELETE works on dolt_tests.",
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_tests;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "select from dolt_tests tables",
            intent: "dolt_tests returns its tests.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('one table', 'validate tables', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '1');",
                "INSERT INTO dolt_tests VALUES ('numbers table exists', 'validate tables', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'' AND tablename LIKE ''numbers''', 'expected_rows', '==', '1');",
                "INSERT INTO dolt_tests VALUES ('numbers schema', 'numbers table validation', 'describe numbers', 'expected_rows', '==', '1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_tests;",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group", TEXT), Column("test_query", TEXT), Column("assertion_type", TEXT), Column("assertion_comparator", TEXT), Column("assertion_value", TEXT)],
                        rows: &[
                            &[T("one table"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("expected_rows"), T("=="), T("1")],
                            &[T("numbers table exists"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public' AND tablename LIKE 'numbers'"), T("expected_rows"), T("=="), T("1")],
                            &[T("numbers schema"), T("numbers table validation"), T("describe numbers"), T("expected_rows"), T("=="), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_tests WHERE test_group = 'validate tables';",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group", TEXT), Column("test_query", TEXT), Column("assertion_type", TEXT), Column("assertion_comparator", TEXT), Column("assertion_value", TEXT)],
                        rows: &[
                            &[T("one table"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("expected_rows"), T("=="), T("1")],
                            &[T("numbers table exists"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public' AND tablename LIKE 'numbers'"), T("expected_rows"), T("=="), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can replace row in dolt_tests table",
            intent: "A test with an existing name replaces the old one.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('one table', 'validate tables', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '1');",
                "INSERT INTO dolt_tests VALUES ('numbers table exists', 'validate tables', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'' AND tablename LIKE ''numbers'';', 'expected_rows', '==', '1');",
                "INSERT INTO dolt_tests VALUES ('numbers table exists', 'validate tables', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'' AND tablename LIKE ''numbers'';', 'expected_rows', '==', '1') ON CONFLICT (test_name) DO UPDATE SET test_group = excluded.test_group, test_query = excluded.test_query, assertion_type = excluded.assertion_type, assertion_comparator = excluded.assertion_comparator, assertion_value = excluded.assertion_value;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_tests;",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group", TEXT), Column("test_query", TEXT), Column("assertion_type", TEXT), Column("assertion_comparator", TEXT), Column("assertion_value", TEXT)],
                        rows: &[
                            &[T("numbers table exists"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public' AND tablename LIKE 'numbers';"), T("expected_rows"), T("=="), T("1")],
                            &[T("one table"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("expected_rows"), T("=="), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can use 'as of' on dolt_tests table",
            intent: "dolt_tests read AS OF a commit shows the tests committed then.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('one table', 'validate tables', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '1');",
                "SELECT dolt_commit('-A','-m', 'first commit');",
                "INSERT INTO dolt_tests VALUES ('numbers table exists', 'validate tables', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'' AND tablename LIKE ''numbers'';', 'expected_rows', '==', '1');",
                "SELECT dolt_commit('-A', '-m', 'second commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_tests AS OF 'HEAD~2';",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group", TEXT), Column("test_query", TEXT), Column("assertion_type", TEXT), Column("assertion_comparator", TEXT), Column("assertion_value", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_tests AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group", TEXT), Column("test_query", TEXT), Column("assertion_type", TEXT), Column("assertion_comparator", TEXT), Column("assertion_value", TEXT)],
                        rows: &[
                            &[T("one table"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("expected_rows"), T("=="), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_tests AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group", TEXT), Column("test_query", TEXT), Column("assertion_type", TEXT), Column("assertion_comparator", TEXT), Column("assertion_value", TEXT)],
                        rows: &[
                            &[T("one table"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("expected_rows"), T("=="), T("1")],
                            &[T("numbers table exists"), T("validate tables"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public' AND tablename LIKE 'numbers';"), T("expected_rows"), T("=="), T("1")],
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

#[test]
fn test_dolt_test_run_function_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "Invalid args into dolt test run",
            intent: "dolt_test_run refuses arguments that name no test or group.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('valid argument', 'argument tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('invalid');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "could not find tests for argument: invalid", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('valid argument', 'invalid');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "could not find tests for argument: invalid", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Delimiter is optional for dolt_test_run",
            intent: "dolt_test_run runs tests named with or without a trailing delimiter.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('should pass', 'delimiter tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public''', 'expected_rows', '==', '0'), ('should also pass', 'delimiter tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('delimiter tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should also pass"), T("delimiter tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("should pass"), T("delimiter tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public'"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Null test group functions correctly",
            intent: "A test with no group runs by name.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('should pass', NULL, 'select * from dolt_log;', 'expected_rows', '>=', '1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('should pass');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should pass"), T(""), T("select * from dolt_log;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Simple row and column tests",
            intent: "expected_rows and expected_columns assertions pass or fail by the query's row and column counts.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('should pass rows', 'row tests', 'select * from dolt_branches;', 'expected_rows', '!=', '4'), ('should fail rows', 'row tests', 'select * from dolt_branches;', 'expected_rows', '==', '4'), ('expect integer for rows', 'row tests', 'select * from dolt_branches;', 'expected_rows', '==', '0.5'), ('should pass columns', 'column tests', 'select * from dolt_branches;', 'expected_columns', '!=', '5'), ('should fail columns', 'column tests', 'select * from dolt_branches;', 'expected_columns', '==', '7'), ('expect integer for columns', 'column tests', 'select * from dolt_branches;', 'expected_columns', '==', '0.5');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('row tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("expect integer for rows"), T("row tests"), T("select * from dolt_branches;"), T("FAIL"), T("cannot run assertion on non integer value: 0.5")],
                            &[T("should fail rows"), T("row tests"), T("select * from dolt_branches;"), T("FAIL"), T("Assertion failed: expected_rows equal to 4, got 1")],
                            &[T("should pass rows"), T("row tests"), T("select * from dolt_branches;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('column tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("expect integer for columns"), T("column tests"), T("select * from dolt_branches;"), T("FAIL"), T("cannot run assertion on non integer value: 0.5")],
                            &[T("should fail columns"), T("column tests"), T("select * from dolt_branches;"), T("FAIL"), T("Assertion failed: expected_columns equal to 7, got 12")],
                            &[T("should pass columns"), T("column tests"), T("select * from dolt_branches;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can expect single integer",
            intent: "An expected_single_value assertion compares an integer result.",
            set_up_script: &[
                "CREATE TABLE test_table (number int);",
                "INSERT INTO test_table VALUES (1);",
                "INSERT INTO dolt_tests VALUES ('should fail', 'single value tests', 'select number from test_table;', 'expected_single_value', '==','5'), ('should pass', 'single value tests', 'select number from test_table;', 'expected_single_value', '==', '1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('single value tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should fail"), T("single value tests"), T("select number from test_table;"), T("FAIL"), T("Assertion failed: expected_single_value equal to 5, got 1")],
                            &[T("should pass"), T("single value tests"), T("select number from test_table;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can expect single string",
            intent: "An expected_single_value assertion compares a string result.",
            set_up_script: &[
                "CREATE TABLE test_table (number text);",
                "INSERT INTO test_table VALUES ('String');",
                "INSERT INTO dolt_tests VALUES ('should pass', 'single value tests', 'select number from test_table;', 'expected_single_value', '==', 'String'), ('should fail', 'single value tests', 'select number from test_table;', 'expected_single_value', '!=', 'String');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('single value tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should fail"), T("single value tests"), T("select number from test_table;"), T("FAIL"), T("Assertion failed: expected_single_value not equal to String, got String")],
                            &[T("should pass"), T("single value tests"), T("select number from test_table;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can expect multiple date formats",
            intent: "An expected_single_value assertion compares date results written in several formats.",
            set_up_script: &[
                "CREATE TABLE test (base DATE, withtime timestamp);",
                "INSERT INTO test VALUES ('2025-08-22', '2025-08-22 09:00:00');",
                "INSERT INTO dolt_tests VALUES ('only date pass', 'base tests', 'select base from test;', 'expected_single_value', '<', '2025-08-23'), ('only date fail', 'base tests', 'select base from test;', 'expected_single_value', '>', '2025-08-23'), ('datetime pass', 'datetime tests', 'select withtime from test;', 'expected_single_value', '<=', '2025-08-22 09:00:00'), ('datetime fail', 'datetime tests', 'select withtime from test;', 'expected_single_value', '>', '2025-08-22 09:00:01');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('base tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("only date fail"), T("base tests"), T("select base from test;"), T("FAIL"), T("Assertion failed: expected_single_value greater than 2025-08-23, got 2025-08-22")],
                            &[T("only date pass"), T("base tests"), T("select base from test;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('datetime tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("datetime fail"), T("datetime tests"), T("select withtime from test;"), T("FAIL"), T("Assertion failed: expected_single_value greater than 2025-08-22 09:00:01, got 2025-08-22 09:00:00")],
                            &[T("datetime pass"), T("datetime tests"), T("select withtime from test;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can expect single float",
            intent: "An expected_single_value assertion compares a float result.",
            set_up_script: &[
                "CREATE TABLE test (a float);",
                "INSERT INTO test VALUES (3.14159);",
                "INSERT INTO dolt_tests VALUES ('should pass', 'single value tests', 'select * from test;', 'expected_single_value', '<', '3.2'), ('should fail', 'single value tests', 'select * from test;', 'expected_single_value', '>', '3.2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('single value tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should fail"), T("single value tests"), T("select * from test;"), T("FAIL"), T("Assertion failed: expected_single_value greater than 3.2, got 3.14159")],
                            &[T("should pass"), T("single value tests"), T("select * from test;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can expect single decimal",
            intent: "An expected_single_value assertion compares a decimal result.",
            set_up_script: &[
                "CREATE TABLE decimals (d DECIMAL(10,2));",
                "INSERT INTO decimals VALUES (10.4);",
                "INSERT INTO dolt_tests VALUES ('should pass', 'decimal tests', 'select * from decimals;', 'expected_single_value', '<', '10.5'), ('should fail', 'decimal tests', 'select * from decimals;', 'expected_single_value', '>', '10.5'), ('can compare to integer', 'decimal tests', 'select * from decimals;', 'expected_single_value', '>', '10');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('decimal tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("can compare to integer"), T("decimal tests"), T("select * from decimals;"), T("PASS"), T("")],
                            &[T("should fail"), T("decimal tests"), T("select * from decimals;"), T("FAIL"), T("Assertion failed: expected_single_value greater than 10.5, got 10.40")],
                            &[T("should pass"), T("decimal tests"), T("select * from decimals;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can expect single boolean",
            intent: "An expected_single_value assertion compares a boolean result.",
            set_up_script: &[
                "CREATE TABLE booleans (b BOOLEAN);",
                "INSERT INTO booleans VALUES (TRUE);",
                "INSERT INTO dolt_tests VALUES ('should pass', 'boolean tests', 'select * from booleans;', 'expected_single_value', '==', 'true'), ('should fail', 'boolean tests', 'select * from booleans;', 'expected_single_value', '==', 'false');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('boolean tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should fail"), T("boolean tests"), T("select * from booleans;"), T("FAIL"), T("Assertion failed: expected_single_value equal to false, got true")],
                            &[T("should pass"), T("boolean tests"), T("select * from booleans;"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can handle null values correctly",
            intent: "Assertions compare NULL results correctly.",
            set_up_script: &[
                "CREATE TABLE numbers (i int, t text, j int);",
                "INSERT INTO numbers VALUES (NULL, NULL, 4);",
                "INSERT INTO dolt_tests (test_name, test_query, assertion_type, assertion_comparator) VALUES ('simple null int equality', 'SELECT i FROM numbers', 'expected_single_value', '=='), ('simple null string equality', 'SELECT t FROM numbers', 'expected_single_value', '=='), ('simple null inequality', 'SELECT i FROM numbers', 'expected_single_value', '!='), ('expect null, get not null', 'SELECT j FROM numbers', 'expected_single_value', '==');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('*');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("expect null, get not null"), T(""), T("SELECT j FROM numbers"), T("FAIL"), T("Assertion failed: expected_single_value equal to NULL, got 4")],
                            &[T("simple null inequality"), T(""), T("SELECT i FROM numbers"), T("FAIL"), T("Assertion failed: expected_single_value not equal to NULL, got NULL")],
                            &[T("simple null int equality"), T(""), T("SELECT i FROM numbers"), T("PASS"), T("")],
                            &[T("simple null string equality"), T(""), T("SELECT t FROM numbers"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Single value will not accept multiple values",
            intent: "An expected_single_value assertion fails on a query that returns several values.",
            set_up_script: &[
                "CREATE TABLE numbers (i int);",
                "INSERT INTO numbers VALUES (1),(2);",
                "INSERT INTO dolt_tests VALUES ('should fail, many columns', 'not one cell', 'select * from dolt_log;', 'expected_single_value', '==', '0');",
                "INSERT INTO dolt_tests VALUES ('should fail, no rows', 'not one cell', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'' AND tablename LIKE ''invalid'';', 'expected_single_value', '==', '0');",
                "INSERT INTO dolt_tests VALUES ('should fail, many rows', 'not one cell', 'select * from numbers;', 'expected_single_value', '==', '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('not one cell');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should fail, many columns"), T("not one cell"), T("select * from dolt_log;"), T("FAIL"), T("expected_single_value expects exactly one cell. Received multiple columns")],
                            &[T("should fail, many rows"), T("not one cell"), T("select * from numbers;"), T("FAIL"), T("expected_single_value expects exactly one cell. Received multiple rows")],
                            &[T("should fail, no rows"), T("not one cell"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public' AND tablename LIKE 'invalid';"), T("FAIL"), T("expected_single_value expects exactly one cell. Received 0 rows")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can run dolt unit tests on each comparison type",
            intent: "Each comparison operator works in passing assertions.",
            set_up_script: &[
                "CREATE TABLE test (i int);",
                "INSERT INTO dolt_tests VALUES ('equal to', 'comparison tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '1'), ('not equal to', 'comparison tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '!=', '2'), ('less than', 'comparison tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '<', '2'), ('less than or equal to', 'comparison tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '<=', '1'), ('greater than', 'comparison tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '>', '0'), ('greater than or equal to', 'comparison tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';','expected_rows', '>=', '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('comparison tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("equal to"), T("comparison tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("greater than"), T("comparison tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("greater than or equal to"), T("comparison tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("less than"), T("comparison tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("less than or equal to"), T("comparison tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("not equal to"), T("comparison tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "expect_single_value with each comparison type, failing assertions",
            intent: "Each comparison operator reports failing expected_single_value assertions.",
            set_up_script: &[
                "CREATE TABLE test (i int, t text);",
                "INSERT INTO test VALUES (1, 'text'), (2, 'text');",
                "INSERT INTO dolt_tests VALUES ('equal to', 'comparison tests', 'select sum(i) from test', 'expected_single_value', '==', '1'),('not equal to', 'comparison tests', 'select sum(i) from test', 'expected_single_value', '!=', '3'),('less than', 'comparison tests', 'select sum(i) from test', 'expected_single_value', '<', '2'),('less than or equal to', 'comparison tests', 'select sum(i) from test', 'expected_single_value', '<=', '2'),('greater than', 'comparison tests', 'select sum(i) from test', 'expected_single_value', '>', '4'),('greater than or equal to', 'comparison tests', 'select sum(i) from test','expected_single_value', '>=', '4');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('comparison tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("equal to"), T("comparison tests"), T("select sum(i) from test"), T("FAIL"), T("Assertion failed: expected_single_value equal to 1, got 3")],
                            &[T("greater than"), T("comparison tests"), T("select sum(i) from test"), T("FAIL"), T("Assertion failed: expected_single_value greater than 4, got 3")],
                            &[T("greater than or equal to"), T("comparison tests"), T("select sum(i) from test"), T("FAIL"), T("Assertion failed: expected_single_value greater than or equal to 4, got 3")],
                            &[T("less than"), T("comparison tests"), T("select sum(i) from test"), T("FAIL"), T("Assertion failed: expected_single_value less than 2, got 3")],
                            &[T("less than or equal to"), T("comparison tests"), T("select sum(i) from test"), T("FAIL"), T("Assertion failed: expected_single_value less than or equal to 2, got 3")],
                            &[T("not equal to"), T("comparison tests"), T("select sum(i) from test"), T("FAIL"), T("Assertion failed: expected_single_value not equal to 3, got 3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "expected_columns with each comparison type, failing assertions",
            intent: "Each comparison operator reports failing expected_columns assertions.",
            set_up_script: &[
                "CREATE TABLE test (i int, t text);",
                "INSERT INTO test VALUES (1, 'text'), (2, 'text');",
                "INSERT INTO dolt_tests VALUES ('equal to', 'column comparison tests', 'select * from test', 'expected_columns', '==', '1'),('not equal to', 'column comparison tests', 'select * from test', 'expected_columns', '!=', '2'),('less than', 'column comparison tests', 'select * from test', 'expected_columns', '<', '1'),('less than or equal to', 'column comparison tests', 'select * from test', 'expected_columns', '<=', '1'),('greater than', 'column comparison tests', 'select * from test', 'expected_columns', '>', '3'),('greater than or equal to', 'column comparison tests', 'select * from test','expected_columns', '>=', '3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('column comparison tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("equal to"), T("column comparison tests"), T("select * from test"), T("FAIL"), T("Assertion failed: expected_columns equal to 1, got 2")],
                            &[T("greater than"), T("column comparison tests"), T("select * from test"), T("FAIL"), T("Assertion failed: expected_columns greater than 3, got 2")],
                            &[T("greater than or equal to"), T("column comparison tests"), T("select * from test"), T("FAIL"), T("Assertion failed: expected_columns greater than or equal to 3, got 2")],
                            &[T("less than"), T("column comparison tests"), T("select * from test"), T("FAIL"), T("Assertion failed: expected_columns less than 1, got 2")],
                            &[T("less than or equal to"), T("column comparison tests"), T("select * from test"), T("FAIL"), T("Assertion failed: expected_columns less than or equal to 1, got 2")],
                            &[T("not equal to"), T("column comparison tests"), T("select * from test"), T("FAIL"), T("Assertion failed: expected_columns not equal to 2, got 2")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can use wildcard, multiple arguments to run all dolt unit tests",
            intent: "dolt_test_run('*') and several arguments run every matching test.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('grouped test', 'wildcard tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '0'), ('second grouped test', 'wildcard tests', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '0'), ('ungrouped test', '', 'SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('*');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("grouped test"), T("wildcard tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("second grouped test"), T("wildcard tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("ungrouped test"), T(""), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run();",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("grouped test"), T("wildcard tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("second grouped test"), T("wildcard tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("ungrouped test"), T(""), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('ungrouped test', 'wildcard tests');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("grouped test"), T("wildcard tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("second grouped test"), T("wildcard tests"), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                            &[T("ungrouped test"), T(""), T("SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("PASS"), T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can not run multiple queries in one test",
            intent: "A test whose query holds several statements fails.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('should fail', '', 'select * from dolt_log; SELECT tablename FROM pg_tables WHERE schemaname = ''public'';', 'expected_rows', '==', '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('should fail');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should fail"), T(""), T("select * from dolt_log; SELECT tablename FROM pg_tables WHERE schemaname = 'public';"), T("FAIL"), T("Can only run exactly one query")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can not test write queries",
            intent: "A test whose query writes fails.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('should fail', '', 'create table test (i int)', 'expected_rows', '==', '1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('should fail');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should fail"), T(""), T("create table test (i int)"), T("FAIL"), T("Cannot execute write queries")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Query errors correctly reported",
            intent: "A test whose query errors reports the error.",
            set_up_script: &[
                "INSERT INTO dolt_tests VALUES ('should fail', '', 'select * from invalid', 'expected_single_value', '==', '1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('should fail');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("should fail"), T(""), T("select * from invalid"), T("FAIL"), T("query error: relation \"invalid\" does not exist")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_run_dolt_tests_validation_tests() {
    run_scripts(&[
        ScriptTest {
            name: "RunDoltTestsValidationTests",
            intent: "dolt_tests refuses an assertion_type or assertion_comparator outside the ones dolt_test_run knows, when the row is inserted.",
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_0', 'grp', 'SELECT 1', 'expected_rows', '==', '0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_1', 'grp', 'SELECT 1', 'expected_rows', '!=', '0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_2', 'grp', 'SELECT 1', 'expected_rows', '<', '0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_3', 'grp', 'SELECT 1', 'expected_rows', '>', '0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_4', 'grp', 'SELECT 1', 'expected_rows', '<=', '0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_5', 'grp', 'SELECT 1', 'expected_rows', '>=', '0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_6', 'grp', 'SELECT 1', 'expected_columns', '==', '0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_7', 'grp', 'SELECT 1', 'expected_single_value', '==', '0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_8', 'grp', 'SELECT 1', 'row_count', '==', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_type_check\"", detail: "Failing row contains (test_8, grp, SELECT 1, row_count, ==, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_type_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_9', 'grp', 'SELECT 1', 'expected_row', '==', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_type_check\"", detail: "Failing row contains (test_9, grp, SELECT 1, expected_row, ==, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_type_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_10', 'grp', 'SELECT 1', 'EXPECTED_ROWS', '==', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_type_check\"", detail: "Failing row contains (test_10, grp, SELECT 1, EXPECTED_ROWS, ==, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_type_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_11', 'grp', 'SELECT 1', 'count', '==', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_type_check\"", detail: "Failing row contains (test_11, grp, SELECT 1, count, ==, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_type_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_12', 'grp', 'SELECT 1', '', '==', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_type_check\"", detail: "Failing row contains (test_12, grp, SELECT 1, , ==, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_type_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_13', 'grp', 'SELECT 1', 'expected_rows_count', '==', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_type_check\"", detail: "Failing row contains (test_13, grp, SELECT 1, expected_rows_count, ==, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_type_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_14', 'grp', 'SELECT 1', 'expected_rows', '=', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_comparator_check\"", detail: "Failing row contains (test_14, grp, SELECT 1, expected_rows, =, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_comparator_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_15', 'grp', 'SELECT 1', 'expected_rows', '===', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_comparator_check\"", detail: "Failing row contains (test_15, grp, SELECT 1, expected_rows, ===, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_comparator_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_16', 'grp', 'SELECT 1', 'expected_rows', 'eq', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_comparator_check\"", detail: "Failing row contains (test_16, grp, SELECT 1, expected_rows, eq, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_comparator_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_17', 'grp', 'SELECT 1', 'expected_rows', '!==', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_comparator_check\"", detail: "Failing row contains (test_17, grp, SELECT 1, expected_rows, !==, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_comparator_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_18', 'grp', 'SELECT 1', 'expected_rows', 'LIKE', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_comparator_check\"", detail: "Failing row contains (test_18, grp, SELECT 1, expected_rows, LIKE, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_comparator_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_19', 'grp', 'SELECT 1', 'expected_rows', '', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_comparator_check\"", detail: "Failing row contains (test_19, grp, SELECT 1, expected_rows, , 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_comparator_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_20', 'grp', 'SELECT 1', 'row_count', '=', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_comparator_check\"", detail: "Failing row contains (test_20, grp, SELECT 1, row_count, =, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_comparator_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test_21', 'grp', 'SELECT 1', 'count', 'eq', '0');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"dolt_tests_assertion_comparator_check\"", detail: "Failing row contains (test_21, grp, SELECT 1, count, eq, 0).", schema: "pg_catalog", table: "dolt_tests", constraint: "dolt_tests_assertion_comparator_check", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
