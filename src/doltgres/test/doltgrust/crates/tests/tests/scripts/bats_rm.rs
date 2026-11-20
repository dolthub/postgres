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

//! Dolt's integration-tests/bats/rm.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_rm() {
    run_scripts(&[
        ScriptTest {
            name: r#"rm: dolt rm without args"#,
            intent: r#"dolt_rm with no tables fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_rm();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"Nothing specified, nothing removed. Which tables should I remove?"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: simple dolt rm"#,
            intent: r#"dolt_rm drops a committed table."#,
            set_up_script: &[
                r#"CREATE TABLE test (i INT);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'created table');"#,
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
                    query: r#"SELECT dolt_rm('test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt ls is a CLI command, so the port reads information_schema.tables"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm nonexistent table"#,
            intent: r#"dolt_rm of a table that doesn't exist fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_rm('nonexistent');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"error: the table(s) nonexistent do not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm multiple tables"#,
            intent: r#"dolt_rm drops several committed tables."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (i INT);"#,
                r#"CREATE TABLE test2 (i INT);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'created tables');"#,
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
                    query: r#"SELECT dolt_rm('test1', 'test2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt ls is a CLI command, so the port reads information_schema.tables"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm multiple tables, some nonexistent"#,
            intent: r#"dolt_rm names every table that doesn't exist."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (i INT);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'created table');"#,
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
                    query: r#"SELECT dolt_rm('test1', 'test2', 'test3');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"error: the table(s) test2, test3 do not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm staged table"#,
            intent: r#"dolt_rm refuses a table with staged changes."#,
            set_up_script: &[
                r#"CREATE TABLE test (i INT);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
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
                    query: r#"SELECT dolt_rm('test');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: the table(s) test have changes saved in the index. Use --cached or commit."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm unstaged table"#,
            intent: r#"dolt_rm refuses a table with unstaged changes."#,
            set_up_script: &[
                r#"CREATE TABLE test (i INT);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_rm('test');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: the table(s) test have unstaged changes."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm with cached option"#,
            intent: r#"dolt_rm --cached stages the table's removal and keeps it in the working set."#,
            set_up_script: &[
                r#"CREATE TABLE test (i INT);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'created table');"#,
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
                    query: r#"SELECT dolt_rm('--cached', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status ORDER BY table_name, staged DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T(r#"public.test"#), T(r#"t"#), T(r#"deleted"#)],
                            &[T(r#"public.test"#), T(r#"f"#), T(r#"new table"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status is a CLI command, so the port reads dolt_status"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm --cached succeeds when a committed table has unstaged changes"#,
            intent: r#"dolt_rm --cached untracks a table with unstaged changes and keeps its rows."#,
            set_up_script: &[
                r#"CREATE TABLE api_keys (id INT PRIMARY KEY, token VARCHAR(64));"#,
                r#"INSERT INTO api_keys VALUES (1, 'initial');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'add api_keys');"#,
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
                    query: r#"INSERT INTO api_keys VALUES (2, 'unstaged-change');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status ORDER BY table_name, staged DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T(r#"public.api_keys"#), T(r#"f"#), T(r#"modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status is a CLI command, so the port reads dolt_status"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_rm('--cached', 'api_keys');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status ORDER BY table_name, staged DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T(r#"public.api_keys"#), T(r#"t"#), T(r#"deleted"#)],
                            &[T(r#"public.api_keys"#), T(r#"f"#), T(r#"new table"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status is a CLI command, so the port reads dolt_status"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM api_keys;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm --cached succeeds when a committed table was dropped from the working set"#,
            intent: r#"dolt_rm --cached stages the removal of a table already dropped."#,
            set_up_script: &[
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (1);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'add test');"#,
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
                    query: r#"DROP TABLE test;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status ORDER BY table_name, staged DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T(r#"public.test"#), T(r#"f"#), T(r#"deleted"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status is a CLI command, so the port reads dolt_status"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_rm('--cached', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status ORDER BY table_name, staged DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T(r#"public.test"#), T(r#"t"#), T(r#"deleted"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status is a CLI command, so the port reads dolt_status"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rm: dolt rm errors on foreign key constrained table"#,
            intent: r#"dolt_rm refuses a table another table's foreign key references."#,
            set_up_script: &[
                r#"CREATE TABLE parent (pk INT PRIMARY KEY, p1 INT);"#,
                r#"CREATE TABLE child (pk INT PRIMARY KEY, c1 INT, FOREIGN KEY (c1) REFERENCES parent (pk));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'created tables');"#,
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
                    query: r#"SELECT dolt_rm('parent');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unable to remove "parent" since it is referenced from table "child""#, ..E }),
                    dolt: Some(Dolt::Differs(r#"Doltgrust quotes the table names as Postgres quotes identifiers, where Dolt uses MySQL's backticks"#)),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
