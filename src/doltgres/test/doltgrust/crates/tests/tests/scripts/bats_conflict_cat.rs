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

//! Dolt's integration-tests/bats/conflict-cat.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_conflict_cat() {
    run_scripts(&[
        ScriptTest {
            name: "conflict-cat: large diff",
            intent: "A conflict's sides keep long text values whole.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, t text);",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT dolt_branch('other');",
                "INSERT INTO t VALUES (1, repeat(' ', 10000));",
                "SELECT dolt_commit('-Am', 'main 10000 spaces');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO t VALUES (1, repeat(' ', 10001));",
                "SELECT dolt_commit('-Am', 'other 10001 spaces');",
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
                    query: "SELECT length(base_t), length(our_t), length(their_t) FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("length", INT4), Column("length", INT4)],
                        rows: &[
                            &[Null, T("10000"), T("10001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-cat: smoke test print schema output",
            intent: "A schema conflict shows the base and each side's column definition.",
            set_up_script: &[
                "CREATE TABLE people (id int NOT NULL, last_name varchar(120), first_name varchar(120), birthday timestamp(6), age int DEFAULT '0', PRIMARY KEY (id));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'base');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "ALTER TABLE people ALTER COLUMN age TYPE real, ALTER COLUMN age DROP NOT NULL, ALTER COLUMN age DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE people ALTER COLUMN age TYPE bigint, ALTER COLUMN age DROP NOT NULL, ALTER COLUMN age DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge right');",
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
                    query: "SELECT description, strpos(our_schema, 'age bigint' || chr(10)) > 0 AS ours, strpos(their_schema, 'age real' || chr(10)) > 0 AS theirs, strpos(base_schema, 'age integer DEFAULT 0' || chr(10)) > 0 AS base FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("description", TEXT), Column("ours", BOOL), Column("theirs", BOOL), Column("base", BOOL)],
                        rows: &[
                            &[T("different column definitions for our column age and their column age"), T("t"), T("t"), T("t")],
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
            name: "conflict-cat: smoke test print data output",
            intent: "Each conflicting row shows its base, ours, and theirs, with how each side changed it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table with rows');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "UPDATE t SET col1 = 3 WHERE pk = 1;",
                "UPDATE t SET col1 = 0 WHERE pk = 2;",
                "DELETE FROM t WHERE pk = 3;",
                "INSERT INTO t VALUES (4, -4);",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 2 WHERE pk = 1;",
                "DELETE FROM t WHERE pk = 2;",
                "UPDATE t SET col1 = 0 WHERE pk = 3;",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_commit('-am', 'left edit');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
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
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, dolt_our_diff_type, their_pk, their_col1, dolt_their_diff_type FROM dolt_conflicts_t ORDER BY coalesce(base_pk, our_pk);",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_col1", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("2"), T("modified"), T("1"), T("3"), T("modified")],
                            &[T("2"), T("2"), Null, Null, T("removed"), T("2"), T("0"), T("modified")],
                            &[T("3"), T("3"), T("3"), T("0"), T("modified"), Null, Null, T("removed")],
                            &[Null, Null, T("4"), T("4"), T("added"), T("4"), T("-4"), T("added")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-cat: conflicts should show using the union-schema (new schema on right)",
            intent: "A conflict on a table whose column one side added shows that column on both sides.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'base');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "ALTER TABLE t ADD c int;",
                "INSERT INTO t VALUES (1, 2, 1);",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 3);",
                "SELECT dolt_commit('-am', 'left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge right');",
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
                    query: "SELECT our_a, our_b, our_c, their_a, their_b, their_c FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("our_a", INT4), Column("our_b", INT4), Column("our_c", INT4), Column("their_a", INT4), Column("their_b", INT4), Column("their_c", INT4)],
                        rows: &[
                            &[T("1"), T("3"), Null, T("1"), T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-cat: conflicts should show using the union-schema (new schema on left)",
            intent: "A conflict on a table whose column one side added shows that column on both sides, when ours added it.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'base');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (1, 2);",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD c int;",
                "INSERT INTO t VALUES (1, 3, 1);",
                "SELECT dolt_commit('-am', 'left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge left');",
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
                    query: "SELECT our_a, our_b, our_c, their_a, their_b, their_c FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("our_a", INT4), Column("our_b", INT4), Column("our_c", INT4), Column("their_a", INT4), Column("their_b", INT4), Column("their_c", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("1"), T("1"), T("2"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_conflicts_<t> shows each side's columns from that side's definition (MERGE_PLAN §7), where Dolt shows the union of the sides' columns"),
            ..S
        },
    ]);
}
