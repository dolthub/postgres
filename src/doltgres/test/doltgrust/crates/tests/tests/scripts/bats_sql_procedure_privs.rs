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

//! Dolt's integration-tests/bats/sql-procedure-privs.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_procedure_privs() {
    run_scripts(&[
        ScriptTest {
            name: r#"sql-procedure-privs: smoke test"#,
            intent: r#"Dolt's functions run without a grant except its admin functions, which need EXECUTE granted by name."#,
            set_up_script: &[
                r#"CREATE USER neil PASSWORD 'pwd';"#,
                r#"CREATE USER mike PASSWORD 'pwd';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO neil;"#,
                    expected: Expected::Tag("GRANT"),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's GRANT ... ON mydb.* is Postgres' grants on the database's schemas and their objects"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) > 0 AS logged FROM dolt_log();"#,
                    expected: Expected::Rows {
                        columns: &[Column("logged", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "neil",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('br1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs(r#"Doltgrust's other Dolt functions keep Postgres' EXECUTE for PUBLIC, so a role runs them without a grant, where Dolt needs EXECUTE on the database"#)),
                    username: "neil",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) > 0 AS logged FROM dolt_log();"#,
                    expected: Expected::Rows {
                        columns: &[Column("logged", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('br2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"neil already made br1 here, so mike makes br2"#)),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_gc"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT EXECUTE ON FUNCTION dolt_gc TO mike;"#,
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "mike",
                    password: "pwd",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-procedure-privs: admin procedures all block"#,
            intent: r#"Every Dolt admin function refuses a role without EXECUTE on it, while other Dolt functions run."#,
            set_up_script: &[
                r#"CREATE USER neil PASSWORD 'pwd';"#,
                r#"CREATE USER mike PASSWORD 'pwd';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'foo');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_backup"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file:///myDatabasesDir/database/.dolt/noms');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_clone"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_fetch"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_gc"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_pull"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_purge_dropped_databases();"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_purge_dropped_databases"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin1', 'Dolthub/museum-collections');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_remote"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop('foo');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_undrop"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('br1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs(r#"Doltgrust's other Dolt functions keep Postgres' EXECUTE for PUBLIC, so a role runs them without a grant, where Dolt needs EXECUTE on the database"#)),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('br1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'br1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "mike",
                    password: "pwd",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-procedure-privs: direct execute privs are let through"#,
            intent: r#"EXECUTE on one admin function lets a role run it and no other admin function."#,
            set_up_script: &[
                r#"CREATE USER neil PASSWORD 'pwd';"#,
                r#"CREATE USER mike PASSWORD 'pwd';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"GRANT EXECUTE ON FUNCTION dolt_gc TO neil;"#,
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "neil",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_fetch"#, ..E }),
                    username: "neil",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('br1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs(r#"Doltgrust's other Dolt functions keep Postgres' EXECUTE for PUBLIC, so a role runs them without a grant, where Dolt needs EXECUTE on the database"#)),
                    username: "neil",
                    password: "pwd",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-procedure-privs: grant option works as explicit grant"#,
            intent: r#"A role with the grant option on an admin function can grant it to another."#,
            set_up_script: &[
                r#"CREATE USER neil PASSWORD 'pwd';"#,
                r#"CREATE USER mike PASSWORD 'pwd';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"GRANT EXECUTE ON FUNCTION dolt_gc TO mike WITH GRANT OPTION;"#,
                    expected: Expected::Tag("GRANT"),
                    dolt: Some(Dolt::Differs(r#"Postgres grants the grant option only with the privilege it is for, so the port grants EXECUTE WITH GRANT OPTION, where Dolt's GRANT OPTION alone lets a user grant without running"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_gc"#, ..E }),
                    username: "neil",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT EXECUTE ON FUNCTION dolt_gc TO neil;"#,
                    expected: Expected::Tag("GRANT"),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "neil",
                    password: "pwd",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-procedure-privs: grant option at DB level does not give admin grant priviliges"#,
            intent: r#"A role without the grant option on an admin function can neither run nor grant it."#,
            set_up_script: &[
                r#"CREATE USER neil PASSWORD 'pwd';"#,
                r#"CREATE USER mike PASSWORD 'pwd';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_gc"#, ..E }),
                    dolt: Some(Dolt::Differs(r#"Postgres has no grant option on a database's functions without EXECUTE on each, so the port grants mike nothing, where Dolt's GRANT OPTION ON mydb.* grants it for the database's non-admin procedures"#)),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT EXECUTE ON FUNCTION dolt_gc TO neil;"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function dolt_gc"#, ..E }),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-procedure-privs: grant option at DB level works for non-admin procedures"#,
            intent: r#"Non-admin Dolt functions run for every role."#,
            set_up_script: &[
                r#"CREATE USER neil PASSWORD 'pwd';"#,
                r#"CREATE USER mike PASSWORD 'pwd';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('br1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs(r#"Postgres has no grant option on a database's functions without EXECUTE on each, so the port grants mike nothing, where Dolt's GRANT OPTION ON mydb.* grants it for the database's non-admin procedures"#)),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('br2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs(r#"Doltgrust's other Dolt functions keep Postgres' EXECUTE for PUBLIC, so a role runs them without a grant, where Dolt needs EXECUTE on the database"#)),
                    username: "neil",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('br3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "neil",
                    password: "pwd",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-procedure-privs: non-dolt procedure execution"#,
            intent: r#"A user's procedure runs for every role, as Postgres grants EXECUTE on routines to PUBLIC."#,
            set_up_script: &[
                r#"CREATE USER neil PASSWORD 'pwd';"#,
                r#"CREATE USER mike PASSWORD 'pwd';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE PROCEDURE user_proc() LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'hello cruel world'; END $$;"#,
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CALL user_proc();"#,
                    expected: Expected::Tag("CALL"),
                    notices: &[Diagnostic { severity: "NOTICE", code: "00000", message: r#"hello cruel world"#, ..N }],
                    dolt: Some(Dolt::MySqlOnly(r#"Postgres grants EXECUTE on a new routine to PUBLIC, where Dolt needs a grant, and a procedure returns no rows, so it raises a notice"#)),
                    username: "mike",
                    password: "pwd",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CALL user_proc();"#,
                    expected: Expected::Tag("CALL"),
                    notices: &[Diagnostic { severity: "NOTICE", code: "00000", message: r#"hello cruel world"#, ..N }],
                    username: "neil",
                    password: "pwd",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
