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

use harness::oid::*;
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_alter_system() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER SYSTEM writes postgresql.auto.conf as Postgres does",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET work_mem = '8MB';"#,
                    expected: Expected::Tag("ALTER SYSTEM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET work_mem = '9MB';"#,
                    expected: Expected::Error(Diagnostic { code: "25001", message: r#"ALTER SYSTEM cannot run inside a transaction block"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET data_directory = 'x';"#,
                    expected: Expected::Error(Diagnostic { code: "55P02", message: r#"parameter "data_directory" cannot be changed"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET allow_alter_system = off;"#,
                    expected: Expected::Error(Diagnostic { code: "55P02", message: r#"parameter "allow_alter_system" cannot be changed"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET work_mem = 'abc';"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "work_mem": "abc""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET work_mem = E'1\n2';"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "work_mem": "1
2""#, hint: r#"Valid units for this parameter are "B", "kB", "MB", "GB", and "TB"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET no_such_param = 1;"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized configuration parameter "no_such_param""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET custom.thing = 'x';"#,
                    expected: Expected::Tag("ALTER SYSTEM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET search_path = a, "b c";"#,
                    expected: Expected::Tag("ALTER SYSTEM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET application_name = 'it''s';"#,
                    expected: Expected::Tag("ALTER SYSTEM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, setting FROM pg_file_settings WHERE sourcefile LIKE '%postgresql.auto.conf' ORDER BY seqno;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT)],
                        rows: &[
                            &[T(r#"work_mem"#), T(r#"8MB"#)],
                            &[T(r#"custom.thing"#), T(r#"x"#)],
                            &[T(r#"search_path"#), T(r#"a, "b c""#)],
                            &[T(r#"application_name"#), T(r#"it's"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM RESET work_mem;"#,
                    expected: Expected::Tag("ALTER SYSTEM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, setting FROM pg_file_settings WHERE sourcefile LIKE '%postgresql.auto.conf' ORDER BY seqno;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT)],
                        rows: &[
                            &[T(r#"custom.thing"#), T(r#"x"#)],
                            &[T(r#"search_path"#), T(r#"a, "b c""#)],
                            &[T(r#"application_name"#), T(r#"it's"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM RESET ALL;"#,
                    expected: Expected::Tag("ALTER SYSTEM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, setting FROM pg_file_settings WHERE sourcefile LIKE '%postgresql.auto.conf' ORDER BY seqno;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE ROLE alice;"#,
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT ALTER SYSTEM ON PARAMETER work_mem TO alice;"#,
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET ROLE alice;"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET work_mem = '16MB';"#,
                    expected: Expected::Tag("ALTER SYSTEM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM SET maintenance_work_mem = '16MB';"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set parameter "maintenance_work_mem""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM RESET ALL;"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to perform ALTER SYSTEM RESET ALL"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RESET ROLE;"#,
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, setting FROM pg_file_settings WHERE sourcefile LIKE '%postgresql.auto.conf' ORDER BY seqno;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT)],
                        rows: &[
                            &[T(r#"work_mem"#), T(r#"16MB"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION f() RETURNS void LANGUAGE plpgsql AS $$ BEGIN ALTER SYSTEM SET work_mem = '1MB'; END $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT f();"#,
                    expected: Expected::Error(Diagnostic { code: "25001", message: r#"ALTER SYSTEM cannot be executed from a function"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SYSTEM RESET ALL;"#,
                    expected: Expected::Tag("ALTER SYSTEM"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
