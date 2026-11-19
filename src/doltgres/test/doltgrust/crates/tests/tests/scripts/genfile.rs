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
fn test_genfile() {
    run_scripts(&[
        ScriptTest {
            name: "the functions that read the server's files and list its directories",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT length(pg_read_file('postgresql.conf')) > 20;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT length(pg_read_file('postgresql.conf', 1, 20));"#,
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T(r#"20"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_file('does not exist');"#,
                    expected: Expected::Error(Diagnostic { code: "58P01", message: r#"could not open file "does not exist" for reading: No such file or directory"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_file('does not exist', true) IS NULL;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_file('does not exist', 0, -1);"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"requested length cannot be negative"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_file('does not exist', 0, -1, true);"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"requested length cannot be negative"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_file('postgresql.conf', -1, 0) = '';"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT length(pg_read_binary_file('postgresql.conf')) > 20;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT length(pg_read_binary_file('postgresql.conf', 1, 20));"#,
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T(r#"20"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_binary_file('does not exist');"#,
                    expected: Expected::Error(Diagnostic { code: "58P01", message: r#"could not open file "does not exist" for reading: No such file or directory"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_binary_file('does not exist', true) IS NULL;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_binary_file('does not exist', 0, -1);"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"requested length cannot be negative"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT size > 20, isdir FROM pg_stat_file('postgresql.conf');"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("isdir", BOOL)],
                        rows: &[
                            &[T(r#"t"#), T(r#"f"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT isdir FROM pg_stat_file('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("isdir", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_stat_file('nope');"#,
                    expected: Expected::Error(Diagnostic { code: "58P01", message: r#"could not stat file "nope": No such file or directory"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_stat_file('nope', true) IS NULL;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_ls_dir('does not exist', false, false);"#,
                    expected: Expected::Error(Diagnostic { code: "58P01", message: r#"could not open directory "does not exist": No such file or directory"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_ls_dir('does not exist', true, false);"#,
                    expected: Expected::Rows {
                        columns: &[Column("pg_ls_dir", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_ls_dir('.', false, true) AS ls WHERE ls = '.';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_ls_dir('.', false, false) AS ls WHERE ls = '.';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM pg_ls_dir('.') a WHERE a = 'pg_hba.conf';"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT)],
                        rows: &[
                            &[T(r#"pg_hba.conf"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) >= 0 AS ok FROM pg_ls_archive_statusdir();"#,
                    expected: Expected::Rows {
                        columns: &[Column("ok", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) >= 0 AS ok FROM pg_ls_summariesdir();"#,
                    expected: Expected::Rows {
                        columns: &[Column("ok", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) >= 0 AS ok FROM pg_ls_tmpdir();"#,
                    expected: Expected::Rows {
                        columns: &[Column("ok", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) >= 0 AS ok FROM pg_ls_tmpdir(1663);"#,
                    expected: Expected::Rows {
                        columns: &[Column("ok", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM pg_ls_tmpdir(42);"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"tablespace with OID 42 does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM pg_ls_logdir();"#,
                    expected: Expected::Error(Diagnostic { code: "58P01", message: r#"could not open directory "log": No such file or directory"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM pg_ls_replslotdir('nope');"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"replication slot "nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_ls_logicalsnapdir();"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("this server keeps no logical decoding snapshots, so their directory doesn't exist and listing it fails as Postgres does for a missing directory"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE ROLE bob;"#,
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT EXECUTE ON FUNCTION pg_read_file(text) TO bob;"#,
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET ROLE bob;"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_file('../x');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"path must be in or below the data directory"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_file('/x');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"absolute path not allowed"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT length(pg_read_file('postgresql.conf')) > 20;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_read_binary_file('postgresql.conf');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for function pg_read_binary_file"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RESET ROLE;"#,
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
