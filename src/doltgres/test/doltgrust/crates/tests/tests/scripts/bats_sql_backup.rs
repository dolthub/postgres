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

//! Dolt's integration-tests/bats/sql-backup.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_backup() {
    run_scripts(&[
        ScriptTest {
            name: r#"sql-backup: dolt_backup no argument"#,
            intent: r#"dolt_backup with no arguments points to dolt_backups."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"use 'dolt_backups' table to list backups"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup add"#,
            intent: r#"dolt_backup('add', ...) adds a backup that dolt_backups lists."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'hostedapidb-0', 'file://{NEWDIR:the_backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, url LIKE '%/the_backup' AS url FROM dolt_backups;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("url", BOOL)],
                        rows: &[
                            &[T(r#"hostedapidb-0"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup add cannot add remote with address of existing backup"#,
            intent: r#"A second backup can't share a backup's address."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'bac1', 'file://{NEWDIR:backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'rem1', 'file://{NEWDIR:backup}');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"address conflict with a remote: 'bac1' -> file://{NEWDIR:backup}"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup remove"#,
            intent: r#"dolt_backup('remove', ...) removes a backup."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'bac1', 'file://{NEWDIR:backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_backups ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"bac1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('remove', 'bac1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_backups ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
            name: r#"sql-backup: dolt_backup remove cannot remove non-existent backup"#,
            intent: r#"Removing a backup that isn't configured fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_backups ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('remove', 'bac1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"backup 'bac1' not found"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup rm"#,
            intent: r#"dolt_backup('rm', ...) removes a backup."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'bac1', 'files://{TEMPDIR}/backup');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_backups ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"bac1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('rm', 'bac1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_backups ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
            name: r#"sql-backup: dolt_backup restore invalid arguments"#,
            intent: r#"dolt_backup('restore', ...) needs exactly a URL and a database name."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"usage: dolt_backup('restore', 'remote_url', 'new_db_name', ['--force'], ['--aws-region=<region>'], ['--aws-creds-type=<type>'], ['--aws-creds-file=<file>'], ['--aws-creds-profile=<profile>'])"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file:///some_directory');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"usage: dolt_backup('restore', 'remote_url', 'new_db_name', ['--force'], ['--aws-region=<region>'], ['--aws-creds-type=<type>'], ['--aws-creds-file=<file>'], ['--aws-creds-profile=<profile>'])"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'hostedapidb-0', 'file:///some_directory', 'too many');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"usage: dolt_backup('restore', 'remote_url', 'new_db_name', ['--force'], ['--aws-region=<region>'], ['--aws-creds-type=<type>'], ['--aws-creds-file=<file>'], ['--aws-creds-profile=<profile>'])"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup restore"#,
            intent: r#"A backup of a dropped database restores under a new name, with its rows and no remotes."#,
            set_up_script: &[
                r#"CREATE DATABASE db1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (42);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'creating table t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'backups', 'file://{NEWDIR:backups}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'backups');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE db1 WITH (FORCE);"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    dolt: Some(Dolt::MySqlOnly(r#"the database's connection from the earlier statements is still open, which Postgres' DROP DATABASE refuses without FORCE"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:backups}', 'db2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname IN ('db1', 'db2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(r#"db2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"42"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db2",
                    database: "db2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_remotes;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "db2",
                    database: "db2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup restore --force on current database"#,
            intent: r#"Restoring a database over itself needs --force and brings back the backed-up rows."#,
            set_up_script: &[
                r#"CREATE DATABASE db1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (42);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'creating table t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'backups', 'file://{NEWDIR:backups}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'backups');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t1 SET pk = 100;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'updating table t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:backups}', 'db1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"database 'db1' already exists, use '--force' to overwrite"#, ..E }),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', '--force', 'file://{NEWDIR:backups}', 'db1');"#,
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot drop the currently open database"#, ..E }),
                    client: "db1",
                    database: "db1",
                    dolt: Some(Dolt::Differs(r#"Postgres refuses to drop the database a session is using, so dolt_backup('restore', '--force', ...) of the session's own database fails and leaves it as it was, where Dolt replaces it (Appendix C, restore over the session's database)"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"100"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    dolt: Some(Dolt::Differs(r#"Postgres refuses to drop the database a session is using, so dolt_backup('restore', '--force', ...) of the session's own database fails and leaves it as it was, where Dolt replaces it (Appendix C, restore over the session's database)"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup restore --force on current database and as cwd"#,
            intent: r#"A session's database restored over itself with --force has the backup's uncommitted rows."#,
            set_up_script: &[
                r#"CREATE DATABASE db1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (i INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3), (4);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync-url', 'file://{NEWDIR:backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:backup}', 'db1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"database 'db1' already exists, use '--force' to overwrite"#, ..E }),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', '--force', 'file://{NEWDIR:backup}', 'db1');"#,
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot drop the currently open database"#, ..E }),
                    client: "db1",
                    database: "db1",
                    dolt: Some(Dolt::Differs(r#"Postgres refuses to drop the database a session is using, so dolt_backup('restore', '--force', ...) of the session's own database fails and leaves it as it was, where Dolt replaces it (Appendix C, restore over the session's database)"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT i FROM t ORDER BY i;"#,
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T(r#"3"#)],
                            &[T(r#"4"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "db1",
                    database: "db1",
                    dolt: Some(Dolt::Differs(r#"Postgres refuses to drop the database a session is using, so dolt_backup('restore', '--force', ...) of the session's own database fails and leaves it as it was, where Dolt replaces it (Appendix C, restore over the session's database)"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup unrecognized"#,
            intent: r#"An unknown dolt_backup subcommand fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('unregonized', 'hostedapidb-0', 'file:///some_directory');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized dolt_backup parameter 'unregonized'"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('unrecognized', 'hostedapidb-0', 'file:///some_directory');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized dolt_backup parameter 'unrecognized'"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup sync wrong number of args"#,
            intent: r#"dolt_backup('sync', ...) needs exactly a backup name."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"usage: dolt_backup('sync', 'name')"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'hostedapidb-0', 'too many');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"usage: dolt_backup('sync', 'name')"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup no such backup"#,
            intent: r#"Syncing a backup that isn't configured fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'hostedapidb-0');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"backup 'hostedapidb-0' not found"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup sync to a backup"#,
            intent: r#"A backup syncs, restores, syncs again with nothing new, and restores again with --force."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'hostedapidb-0', 'file://{NEWDIR:the_backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'hostedapidb-0');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:the_backup}', 'the_restore');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "the_restore",
                    database: "the_restore",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'hostedapidb-0');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'hostedapidb-0');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:the_backup}', 'the_restore', '--force');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "the_restore_again",
                    database: "the_restore",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'hostedapidb-0');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup sync-url"#,
            intent: r#"A backup made by URL restores, and again with --force after another sync."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync-url', 'file://{NEWDIR:the_backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:the_backup}', 'the_restore');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "the_restore",
                    database: "the_restore",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync-url', 'file://{NEWDIR:the_backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:the_backup}', 'the_restore', '--force');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "the_restore_again",
                    database: "the_restore",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup sync-url fails on non-grpc http request"#,
            intent: r#"A backup to an HTTP address that isn't a Dolt remote fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync-url', 'http://dolthub.com/dolthub/backup');"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"dolt_backup to http://dolthub.com/dolthub/backup is not supported: Doltgrust reaches only file remotes"#, ..E }),
                    dolt: Some(Dolt::Differs(r#"Dolt fails with its RPC error from the address, and Doltgrust, which reaches only file remotes, refuses it first"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync-url', 'https://dolthub.com/dolthub/backup');"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"dolt_backup to https://dolthub.com/dolthub/backup is not supported: Doltgrust reaches only file remotes"#, ..E }),
                    dolt: Some(Dolt::Differs(r#"Dolt fails with its RPC error from the address, and Doltgrust, which reaches only file remotes, refuses it first"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup rejects AWS parameters fails in sql-server"#,
            intent: r#"dolt_backup refuses AWS parameters in a server."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'backup1', 'aws://[table:bucket]/db', '--aws-region=us-east-1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"AWS parameters are unavailable when running in server mode"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync-url', 'backup2', 'aws://[table:bucket]/db', '--aws-creds-type=file');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"AWS parameters are unavailable when running in server mode"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'backup3', 'aws://[table:bucket]/db', '--aws-creds-file=/path/to/file');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"AWS parameters are unavailable when running in server mode"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'backup4', 'aws://[table:bucket]/db', '--aws-creds-profile=profile');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"AWS parameters are unavailable when running in server mode"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup works in invalid dolt repository"#,
            intent: r#"A backup restores into a new database with the source's uncommitted rows."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (i INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (4), (3);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync-url', 'file://{NEWDIR:t_backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:t_backup}', 't_db');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI restores from outside any repository, and a SQL restore always runs in a server"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT i FROM t ORDER BY i;"#,
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T(r#"3"#)],
                            &[T(r#"4"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "t_db",
                    database: "t_db",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: server sees restored auto_increment value for inserts after cli restore"#,
            intent: r#"A restored table's identity continues from where the backup's left off."#,
            set_up_script: &[
                r#"CREATE DATABASE ai_src;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (id INT GENERATED BY DEFAULT AS IDENTITY (START WITH 449) PRIMARY KEY, name VARCHAR(20));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "ai_src",
                    database: "ai_src",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's AUTO_INCREMENT=449 is an identity starting at 449"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 'existing');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "ai_src",
                    database: "ai_src",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "ai_src",
                    database: "ai_src",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync-url', 'file://{NEWDIR:ai_backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "ai_src",
                    database: "ai_src",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:ai_backup}', 'ai_restored');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (name) VALUES ('after_restore');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "ai_restored",
                    database: "ai_restored",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T(r#"1"#), T(r#"existing"#)],
                            &[T(r#"449"#), T(r#"after_restore"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "ai_restored",
                    database: "ai_restored",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup --prune-with-grace-period rejects bad values and unrelated subcommands"#,
            intent: r#"--prune-with-grace-period needs a valid duration of at least ten minutes and works only with sync and sync-url."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'hostedapidb-0', 'file://{NEWDIR:the_backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'hostedapidb-0', '--prune-with-grace-period', '5m');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"--prune-with-grace-period must be at least 10m0s, got 5m0s"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'hostedapidb-0', '--prune-with-grace-period', 'nonsense');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for --prune-with-grace-period: time: invalid duration "nonsense""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'bac2', 'file://{NEWDIR:the_backup}-2', '--prune-with-grace-period', '1h');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"--prune-with-grace-period is only supported with 'sync' and 'sync-url'"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('remove', 'hostedapidb-0', '--prune-with-grace-period', '1h');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"--prune-with-grace-period is only supported with 'sync' and 'sync-url'"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-backup: dolt_backup sync does not commit the session's open transaction"#,
            intent: r#"A sync inside a transaction backs up what was committed before it and leaves the transaction's own rows to its rollback."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'b1', 'file://{NEWDIR:backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'init');"#,
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
                    query: r#"INSERT INTO t VALUES (42);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'b1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                            &[T(r#"42"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('restore', 'file://{NEWDIR:backup}', 'the_restore');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM t ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                            &[T(r#"42"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "the_restore",
                    database: "the_restore",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
