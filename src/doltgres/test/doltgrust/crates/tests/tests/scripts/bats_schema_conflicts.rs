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

//! Dolt's integration-tests/bats/schema-conflicts.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_schema_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "schema-conflicts: dolt_schema_conflicts smoke test",
            intent: "dolt_schema_conflicts has its columns and no rows without a merge.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("base_schema", TEXT), Column("our_schema", TEXT), Column("their_schema", TEXT), Column("description", TEXT)],
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
            name: "schema-conflicts: sql merge, query schema conflicts",
            intent: "A merge whose sides change a column to different types records a schema conflict showing each side's type.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE varchar(20);",
                "SELECT dolt_commit('-am', 'alter table t on branch main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE timestamp(6) USING NULL;",
                "SELECT dolt_commit('-am', 'alter table t on branch other');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strpos(our_schema, 'c0 character varying(20)') > 0 AS ours, strpos(their_schema, 'c0 timestamp(6) without time zone') > 0 AS theirs FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("ours", BOOL), Column("theirs", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_conflicts shows each side's schema as pg_dump's statements for the table, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            skip: Some("the merge's conflicts raise Doltgrust's WARNING merge has unresolved conflicts or constraint violations, which the port doesn't expect yet"),
            ..S
        },
        ScriptTest {
            name: "schema-conflicts: cli merge, query schema conflicts",
            intent: "A schema conflict stops a merge, and once kept it shows each side's type.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE varchar(20);",
                "SELECT dolt_commit('-am', 'alter table t on branch main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE timestamp(6) USING NULL;",
                "SELECT dolt_commit('-am', 'alter table t on branch other');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strpos(our_schema, 'c0 character varying(20)') > 0 AS ours, strpos(their_schema, 'c0 timestamp(6) without time zone') > 0 AS theirs FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("ours", BOOL), Column("theirs", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_conflicts shows each side's schema as pg_dump's statements for the table, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            skip: Some("the merge's conflicts raise Doltgrust's WARNING merge has unresolved conflicts or constraint violations, which the port doesn't expect yet"),
            ..S
        },
        ScriptTest {
            name: "schema-conflicts: resolve schema conflict with 'ours' via SQL",
            intent: "Resolving a schema conflict with --ours keeps that side's column type.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE varchar(20);",
                "SELECT dolt_commit('-am', 'alter table t on branch main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE timestamp(6) USING NULL;",
                "SELECT dolt_commit('-am', 'alter table t on branch other');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) AS type FROM pg_attribute WHERE attrelid = 't'::regclass AND attname = 'c0';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("character varying(20)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-conflicts: resolve schema conflict with 'theirs' via SQL",
            intent: "Resolving a schema conflict with --theirs keeps that side's column type.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE varchar(20);",
                "SELECT dolt_commit('-am', 'alter table t on branch main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE timestamp(6) USING NULL;",
                "SELECT dolt_commit('-am', 'alter table t on branch other');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) AS type FROM pg_attribute WHERE attrelid = 't'::regclass AND attname = 'c0';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("timestamp(6) without time zone")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-conflicts: resolve schema conflict with 'ours' via CLI",
            intent: "Resolving a schema conflict with --ours keeps that side's column type.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE varchar(20);",
                "SELECT dolt_commit('-am', 'alter table t on branch main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE timestamp(6) USING NULL;",
                "SELECT dolt_commit('-am', 'alter table t on branch other');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) AS type FROM pg_attribute WHERE attrelid = 't'::regclass AND attname = 'c0';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("character varying(20)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-conflicts: resolve schema conflict with 'theirs' via CLI",
            intent: "Resolving a schema conflict with --theirs keeps that side's column type.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "SELECT dolt_commit('-Am', 'new table t');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE varchar(20);",
                "SELECT dolt_commit('-am', 'alter table t on branch main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE timestamp(6) USING NULL;",
                "SELECT dolt_commit('-am', 'alter table t on branch other');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) AS type FROM pg_attribute WHERE attrelid = 't'::regclass AND attname = 'c0';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("timestamp(6) without time zone")],
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
