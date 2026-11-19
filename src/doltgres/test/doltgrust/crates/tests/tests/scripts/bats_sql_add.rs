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

//! Dolt's integration-tests/bats/sql-add.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Dolt, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_add() {
    run_scripts(&[
        ScriptTest {
            name: "sql-add: DOLT_ADD all flag works",
            intent: "dolt_add with -A stages every table, so a commit leaves nothing changed and carries the configured author.",
            set_up_script: &[
                "SET dolt_author_name = 'Bats Tests';",
                "SET dolt_author_email = 'bats@email.fake';",
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "CREATE TABLE test2 (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'Commit1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Commit1"), T("Bats Tests"), T("bats@email.fake")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-add: DOLT_ADD all w/ . works",
            intent: "dolt_add with . stages every table, so a commit leaves nothing changed and carries the configured author.",
            set_up_script: &[
                "SET dolt_author_name = 'Bats Tests';",
                "SET dolt_author_email = 'bats@email.fake';",
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "CREATE TABLE test2 (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Commit1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Commit1"), T("Bats Tests"), T("bats@email.fake")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-add: DOLT_ADD all w/ . combined with DOLT_COMMIT -a works",
            intent: "dolt_add with . followed by dolt_commit -a commits every table and leaves nothing changed.",
            set_up_script: &[
                "SET dolt_author_name = 'Bats Tests';",
                "SET dolt_author_email = 'bats@email.fake';",
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "CREATE TABLE test2 (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Commit1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Commit1"), T("Bats Tests"), T("bats@email.fake")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-add: DOLT_ADD can take in one table",
            intent: "dolt_add of one table stages only it, so the other new table stays unstaged after the commit.",
            set_up_script: &[
                "SET dolt_author_name = 'Bats Tests';",
                "SET dolt_author_email = 'bats@email.fake';",
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "CREATE TABLE test2 (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Commit1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Commit1"), T("Bats Tests"), T("bats@email.fake")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-add: DOLT_ADD can take in multiple tables",
            intent: "dolt_add of several tables stages them all, so a commit leaves nothing changed.",
            set_up_script: &[
                "SET dolt_author_name = 'Bats Tests';",
                "SET dolt_author_email = 'bats@email.fake';",
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "CREATE TABLE test2 (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('test', 'test2');",
                "SELECT dolt_commit('-m', 'Commit1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Commit1"), T("Bats Tests"), T("bats@email.fake")],
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
