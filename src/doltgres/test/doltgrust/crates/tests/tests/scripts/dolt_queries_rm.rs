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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_rm.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_rm_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt Rm without tables",
            intent: "dolt_rm without a table refuses to remove anything.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Nothing specified, nothing removed. Which tables should I remove?", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "simple dolt Rm",
            intent: "dolt_rm drops a committed table and stages its removal.",
            set_up_script: &[
                "CREATE TABLE test (i int);",
                "SELECT dolt_commit('-A', '-m', 'created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm multiple tables",
            intent: "dolt_rm drops several committed tables at once.",
            set_up_script: &[
                "CREATE TABLE test1 (i int);",
                "CREATE TABLE test2 (i int);",
                "SELECT dolt_commit('-A', '-m', 'created tables');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('test1', 'test2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
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
            name: "dolt Rm staged table",
            intent: "dolt_rm refuses a table whose changes are staged.",
            set_up_script: &[
                "CREATE TABLE test (i int);",
                "SELECT dolt_add('test');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: the table(s) test have changes saved in the index. Use --cached or commit.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm unstaged table",
            intent: "dolt_rm refuses a table with unstaged changes.",
            set_up_script: &[
                "CREATE TABLE test (i int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: the table(s) test have unstaged changes.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm staged table with unstaged changes",
            intent: "dolt_rm refuses a staged table that has more unstaged changes.",
            set_up_script: &[
                "CREATE TABLE test (i int);",
                "SELECT dolt_add('test');",
                "INSERT INTO test VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: the table(s) test have unstaged changes.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm committed table with unstaged changes",
            intent: "dolt_rm refuses a committed table with unstaged changes.",
            set_up_script: &[
                "CREATE TABLE test (i int);",
                "SELECT dolt_commit('-A', '-m', 'created table');",
                "INSERT INTO test VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: the table(s) test have unstaged changes.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm with cached option",
            intent: "dolt_rm --cached stages a table's removal and keeps the table in the working set.",
            set_up_script: &[
                "CREATE TABLE test (i int);",
                "SELECT dolt_commit('-A', '-m', 'created table');",
                "SELECT dolt_rm('test', '--cached');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("deleted")],
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm staged table with cached option",
            intent: "dolt_rm --cached unstages a newly staged table and keeps it in the working set.",
            set_up_script: &[
                "CREATE TABLE test (i int);",
                "SELECT dolt_add('test');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('test', '--cached');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm staged and unstaged with cached option",
            intent: "dolt_rm --cached removes a table with staged and unstaged changes from the index only.",
            set_up_script: &[
                "CREATE TABLE committed (i int);",
                "SELECT dolt_commit('-A', '-m', 'created table');",
                "CREATE TABLE staged (i int);",
                "SELECT dolt_add('staged');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('committed','staged','--cached');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.committed"), T("t"), T("deleted")],
                            &[T("public.staged"), T("f"), T("new table")],
                            &[T("public.committed"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm errors on foreign key constrained table",
            intent: "dolt_rm refuses a table another table's foreign key references.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY, p1 int);",
                "CREATE TABLE child (pk int PRIMARY KEY, c1 int, FOREIGN KEY (c1) REFERENCES parent (pk));",
                "SELECT dolt_commit('-A', '-m', 'created tables');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('parent');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to remove \"parent\" since it is referenced from table \"child\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm errors on staged foreign key constrained table with cached option",
            intent: "dolt_rm --cached refuses a staged table another table's foreign key references.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY, p1 int);",
                "SELECT dolt_commit('-A', '-m', 'created tables');",
                "CREATE TABLE child (pk int PRIMARY KEY, c1 int, FOREIGN KEY (c1) REFERENCES parent (pk));",
                "SELECT dolt_add('child');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('parent','--cached');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to remove \"parent\" since it is referenced from table \"child\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt Rm errors on unstaged foreign key constrained table with cached option",
            intent: "dolt_rm --cached refuses an unstaged table another table's foreign key references.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY, p1 int);",
                "SELECT dolt_commit('-A', '-m', 'created tables');",
                "CREATE TABLE child (pk int PRIMARY KEY, c1 int, FOREIGN KEY (c1) REFERENCES parent (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('parent', '--cached');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to remove \"parent\" since it is referenced from table \"child\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_rm_read_only_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt Rm returns error for read-only database",
            intent: "dolt_rm refuses to run on a read-only database.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('t');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to rm in read-only databases", ..E }),
                    ..A
                },
            ],
            skip: Some("2f: read-only databases (Dolt's harness makes the database read-only; the database access mode of STORAGE_DESIGN Part V)"),
            ..S
        },
    ]);
}
