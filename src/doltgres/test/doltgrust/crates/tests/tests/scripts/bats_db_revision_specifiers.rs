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

//! Dolt's integration-tests/bats/db-revision-specifiers.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_db_revision_specifiers() {
    run_scripts(&[
        ScriptTest {
            name: "db-revision-specifiers: branch-qualified database revisions",
            intent: "A branch's revision database is a connection's database on that branch, its rows read and written there, and names the branch's tables in a qualified name.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, color varchar(200));",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'Created table');",
                "INSERT INTO test VALUES (1, 'green');",
                "SELECT dolt_commit('-am', 'Inserted 1, green');",
                "INSERT INTO test VALUES (2, 'blue');",
                "SELECT dolt_commit('-am', 'Inserted 2, blue');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO test VALUES (3, 'purple');",
                "SELECT dolt_commit('-am', 'Inserted 3, purple');",
                "DELETE FROM test;",
                "SELECT dolt_commit('-am', 'Deleted all rows');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_database(), active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("postgres"), T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::Differs("current_database() names the base database, as pg_database does, and current_branch() names the revision, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("green")],
                            &[T("2"), T("blue")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c1",
                    database: "postgres/branch1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch1\".public.test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("green")],
                            &[T("2"), T("blue")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (100, 'beige');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "c2",
                    database: "postgres/branch1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "db-revision-specifiers: tag-qualified database revisions",
            intent: "A tag's revision database reads the tag's commit as a connection's database and in a qualified name, and refuses writes.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, color varchar(200));",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'Created table');",
                "INSERT INTO test VALUES (1, 'green');",
                "SELECT dolt_commit('-am', 'Inserted 1, green');",
                "SELECT dolt_tag('v1');",
                "INSERT INTO test VALUES (2, 'blue');",
                "SELECT dolt_commit('-am', 'Inserted 2, blue');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO test VALUES (3, 'purple');",
                "SELECT dolt_commit('-am', 'Inserted 3, purple');",
                "DELETE FROM test;",
                "SELECT dolt_commit('-am', 'Deleted all rows');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("green")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/v1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/v1",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/v1\".public.test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("green")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (100, 'beige');",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    client: "c2",
                    database: "postgres/v1",
                    dolt: Some(Dolt::Differs("Postgres refuses INSERT in a read-only transaction with its own text, where Dolt names the read-only revision database (Appendix C \"revision connections\")")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "db-revision-specifiers: commit-qualified database revisions",
            intent: "A commit's hash names the revision whose rows are read.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, color varchar(200));",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'Created table');",
                "INSERT INTO test VALUES (1, 'green');",
                "SELECT dolt_commit('-am', 'Inserted 1, green');",
                "INSERT INTO test VALUES (2, 'blue');",
                "SELECT dolt_commit('-am', 'Inserted 2, blue');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO test VALUES (3, 'purple');",
                "SELECT dolt_commit('-am', 'Inserted 3, purple');",
                "DELETE FROM test;",
                "SELECT dolt_commit('-am', 'Deleted all rows');",
                "SELECT set_config('test.commit', dolt_hashof('HEAD~1'), false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF current_setting('test.commit') ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("green")],
                            &[T("2"), T("blue")],
                            &[T("3"), T("purple")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("the test names the commit's revision database by a hash it computes, which a client's connection can't name, so the port reads the commit AS OF the hash, and the read-only check of that database has no form here")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
