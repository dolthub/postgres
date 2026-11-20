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

//! Dolt's integration-tests/bats/sql-commit.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_commit() {
    run_scripts(&[
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT without a message throws error",
            intent: "dolt_commit without a message is refused and makes no commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Must provide commit message.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT with just a message reads session parameters",
            intent: "dolt_commit with only a message commits as the session's identity.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Commit1');",
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
                    query: "SELECT committer, email FROM dolt_log WHERE message = 'Commit1';",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T("postgres"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's bats commit as Bats Tests <bats@email.fake>, where Doltgrust commits as the session's user")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT with the all flag performs properly",
            intent: "dolt_commit with -a commits every change.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '-m', 'Commit1');",
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
                    query: "SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff with no output is an empty diff")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT committer FROM dolt_log WHERE message = 'Commit1';",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's bats commit as Bats Tests <bats@email.fake>, where Doltgrust commits as the session's user")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT with all flag, message and author",
            intent: "dolt_commit with an author records it, returns the new head's hash, and lists the commit in dolt_commits.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.dcommit', dolt_commit('-a', '-m', 'Commit1', '--author', 'John Doe <john@doe.com>'), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff with no output is an empty diff")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT committer, email FROM dolt_log WHERE message = 'Commit1';",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T("John Doe"), T("john@doe.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.dcommit') FROM dolt_log ORDER BY date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commits WHERE message = 'Commit1';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT works with --author without config variables set",
            intent: "dolt_commit with an author needs no configured identity.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.dcommit', dolt_commit('-m', 'Commit1', '--author', 'John Doe <john@doe.com>'), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt config --unset of the global identity has no SQL form, and the session's user stands for it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT committer, email FROM dolt_log WHERE message = 'Commit1';",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T("John Doe"), T("john@doe.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.dcommit') FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commits WHERE message = 'Commit1';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT immediately updates dolt log system table.",
            intent: "A commit shows in dolt_log in the same session.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '-m', 'Commit1');",
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
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'Commit1';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT immediately updates dolt diff system table.",
            intent: "dolt_diff_<t> shows the new commit's change from the previous head in the same session.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.original', dolt_hashof('HEAD'), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '-m', 'Commit1');",
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
                    query: "SELECT DISTINCT dolt_from_commit = current_setting('test.original') FROM dolt_diff_test WHERE dolt_to_commit = dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT updates session variables",
            intent: "A commit moves the session's head to the new commit, which heads the log.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', dolt_hashof('HEAD'), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '-m', 'Commit1');",
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
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = (SELECT commit_hash FROM dolt_log LIMIT 1);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: DOLT_COMMIT with unstaged tables leaves them in the working set",
            intent: "A commit of staged tables leaves unstaged tables and changes in the working set.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', '0, 1, 2 in test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's @@<database>_head session variable is the session's head, which dolt_hashof('HEAD') reads")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '1');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("0, 1, 2 in test")],
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
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' AND table_name = 'test2';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's; information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added test2 table');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's @@<database>_head session variable is the session's head, which dolt_hashof('HEAD') reads")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('-n', '1');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added test2 table")],
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
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: The -f parameter is properly parsed and executes",
            intent: "dolt_commit with -f commits tables whose foreign keys are violated.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's SET FOREIGN_KEY_CHECKS=0 is session_replication_role = replica")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE colors (id int NOT NULL, color varchar(32) NOT NULL, PRIMARY KEY (id), CONSTRAINT color_index UNIQUE (color));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("A Postgres foreign key needs a unique key where MySQL's takes a plain index")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE objects (id int NOT NULL, name varchar(64) NOT NULL, color varchar(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-fam', 'Commit1');",
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
                    query: "SELECT count(*) FROM objects;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: missing message does not panic and throws an error",
            intent: "dolt_commit with -m's flag and no message is refused.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-fam');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: no value for option `message'", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: --skip-empty correctly skips committing when no changes are staged",
            intent: "--skip-empty refuses --allow-empty, commits staged changes, and makes no commit when nothing is staged.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.original', dolt_hashof('HEAD'), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '--skip-empty', '-m', 'commit message');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: cannot use both --allow-empty and --skip-empty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.original');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--skip-empty', '-m', 'commit message');",
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
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.original');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.new', dolt_hashof('HEAD'), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--skip-empty', '-m', 'commit message');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.new');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-commit: dolt_author_name and dolt_committer_name session variables set commit identity",
            intent: "The dolt_author_* and dolt_committer_* settings set a commit's author and committer.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t_sess_ident (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t_sess_ident');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_author_name = 'SQL Author';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_author_email = 'sql_author@test.com';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_name = 'SQL Committer';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_committer_email = 'sql_committer@test.com';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'session var commit');",
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
                    query: "SELECT author, author_email, committer, email FROM dolt_log WHERE message = 'session var commit';",
                    expected: Expected::Rows {
                        columns: &[Column("author", TEXT), Column("author_email", TEXT), Column("committer", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T("SQL Author"), T("sql_author@test.com"), T("SQL Committer"), T("sql_committer@test.com")],
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
