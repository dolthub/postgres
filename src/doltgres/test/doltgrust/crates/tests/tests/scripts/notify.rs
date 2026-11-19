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
fn test_listen_and_notify() {
    run_scripts(&[
        ScriptTest {
            name: "LISTEN, UNLISTEN, NOTIFY, and the notification functions check their arguments and keep the channels a session's commits leave",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "LISTEN a;",
                    expected: Expected::Tag("LISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"LISTEN "B c";"#,
                    expected: Expected::Tag("LISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_listening_channels() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_listening_channels", TEXT)],
                        rows: &[
                            &[T("B c")],
                            &[T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "NOTIFY n, 'one';",
                    expected: Expected::Tag("NOTIFY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify('n', 'two');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_notify", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"NOTIFY "";"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"zero-length delimited identifier at or near """""#, position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify('', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "channel name cannot be empty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify(NULL, 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "channel name cannot be empty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify('n', NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_notify", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify(repeat('x', 64), 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "channel name too long", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify(repeat('x', 63), 'x');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_notify", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify('n', repeat('x', 8000));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "payload string too long", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notify('n', repeat('x', 7999));",
                    expected: Expected::Rows {
                        columns: &[Column("pg_notify", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN READ ONLY;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "NOTIFY n;",
                    expected: Expected::Tag("NOTIFY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LISTEN z;",
                    expected: Expected::Tag("LISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UNLISTEN z;",
                    expected: Expected::Tag("UNLISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LISTEN later;",
                    expected: Expected::Tag("LISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_listening_channels() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_listening_channels", TEXT)],
                        rows: &[
                            &[T("B c")],
                            &[T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_listening_channels() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_listening_channels", TEXT)],
                        rows: &[
                            &[T("B c")],
                            &[T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LISTEN sub;",
                    expected: Expected::Tag("LISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LISTEN kept;",
                    expected: Expected::Tag("LISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_listening_channels() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_listening_channels", TEXT)],
                        rows: &[
                            &[T("B c")],
                            &[T("a")],
                            &[T("kept")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UNLISTEN a;",
                    expected: Expected::Tag("UNLISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_listening_channels() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_listening_channels", TEXT)],
                        rows: &[
                            &[T("B c")],
                            &[T("kept")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UNLISTEN *;",
                    expected: Expected::Tag("UNLISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_listening_channels() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_listening_channels", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_notification_queue_usage();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_notification_queue_usage", FLOAT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UNLISTEN nothing;",
                    expected: Expected::Tag("UNLISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LISTEN p;",
                    expected: Expected::Tag("LISTEN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'x';",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "prepared transactions are disabled", hint: r#"Set "max_prepared_transactions" to a nonzero value."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_listening_channels() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_listening_channels", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "NOTIFY p, 'q';",
                    expected: Expected::Tag("NOTIFY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'x';",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "prepared transactions are disabled", hint: r#"Set "max_prepared_transactions" to a nonzero value."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_notify_rule_actions() {
    run_scripts(&[
        ScriptTest {
            name: "a NOTIFY rule action runs whatever the query's condition and prints as NOTIFY",
            set_up_script: &[
                "CREATE TABLE nt (x INT);",
                "INSERT INTO nt VALUES (1), (2);",
                "CREATE RULE nr AS ON UPDATE TO nt DO ALSO NOTIFY nch;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE nt SET x = x + 10 WHERE x > 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x FROM nt ORDER BY x;",
                    expected: Expected::Rows { columns: &[Column("x", INT4)], rows: &[&[T("1")], &[T("12")]], tag: "SELECT 2" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_ruledef(oid) FROM pg_rewrite WHERE rulename = 'nr';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_ruledef", TEXT)],
                        rows: &[&[T("CREATE RULE nr AS\n    ON UPDATE TO public.nt DO\n NOTIFY nch;")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
