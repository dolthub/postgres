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

//! Dolt's integration-tests/bats/conflict-detection.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_conflict_detection() {
    run_scripts(&[
        ScriptTest {
            name: "conflict-detection: merge non-existent branch errors",
            intent: "Merging a branch that does not exist fails.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('batmans-parents');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: batmans-parents", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: cannot merge into dirty working table",
            intent: "A merge that would overwrite a table with uncommitted changes fails, and merges once they are committed.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "UPDATE test SET c5 = 11 WHERE pk = 1;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=1 c5 to 11');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET c1 = 11 WHERE pk = 0;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: local changes would be stomped by merge:\n\tpublic.test\n Please commit your changes before you merge.", ..E }),
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
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
                    query: "SELECT length(dolt_commit('-m', 'changes pk=0 c1 t0 11')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("11"), T("0"), T("0"), T("0"), T("0")],
                            &[T("1"), T("1"), T("1"), T("1"), T("1"), T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches modify different cell different row. merge. no conflict",
            intent: "Changes to different rows merge without conflicts, leaving the merge staged to commit.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('change-cell');",
                "UPDATE test SET c1 = 11 WHERE pk = 0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=0 c1 to 11');",
                "SELECT * FROM dolt_checkout('change-cell');",
                "UPDATE test SET c5 = 11 WHERE pk = 1;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=1 c5 to 11');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('change-cell', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("11"), T("0"), T("0"), T("0"), T("0")],
                            &[T("1"), T("1"), T("1"), T("1"), T("1"), T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging FROM dolt_merge_status;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches modify different cell same row. merge. no conflict",
            intent: "Changes to different cells of one row merge without conflicts, leaving the merge staged to commit.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('change-cell');",
                "UPDATE test SET c1 = 11 WHERE pk = 0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=0 c1 to 11');",
                "SELECT * FROM dolt_checkout('change-cell');",
                "UPDATE test SET c5 = 11 WHERE pk = 0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=0 c5 to 11');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('change-cell', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("11"), T("0"), T("0"), T("0"), T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging FROM dolt_merge_status;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches modify same cell. merge. conflict",
            intent: "Different changes to the same cells are a conflict that leaves the table unmerged.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('change-cell');",
                "UPDATE test SET c1 = 1, c2 = 1, c3 = 1, c4 = 1, c5 = 1 WHERE pk = 0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=0 all cells to 1');",
                "SELECT * FROM dolt_checkout('change-cell');",
                "UPDATE test SET c1 = 11, c2 = 11, c3 = 11, c4 = 11, c5 = 11 WHERE pk = 0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=0 all cells to 11');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('change-cell', '-m', 'merge');",
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
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches add a different row. merge. no conflict",
            intent: "Rows added with different keys merge.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('add-row');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added pk=0 row');",
                "SELECT * FROM dolt_checkout('add-row');",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added pk=1 row');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('add-row', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                            &[T("1"), T("1"), T("1"), T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches add same row. merge. no conflict",
            intent: "The same row added on both sides merges.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('add-row');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added pk=0 row');",
                "SELECT * FROM dolt_checkout('add-row');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added pk=0 row');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('add-row', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: one branch add table, other modifies table. merge. no conflict",
            intent: "A table added on one side merges with rows added to another table on the other.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('add-table');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added row');",
                "SELECT * FROM dolt_checkout('add-table');",
                "CREATE TABLE test2 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test2');",
                "SELECT dolt_commit('-m', 'added new table test2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('add-table', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_regclass('test2') IS NOT NULL AS exists;",
                    expected: Expected::Rows {
                        columns: &[Column("exists", BOOL)],
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
            name: "conflict-detection: two branches add same column. merge. no conflict",
            intent: "The same column added on both sides merges.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('add-column');",
                "ALTER TABLE test ADD c0 bigint;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added column c0');",
                "SELECT * FROM dolt_checkout('add-column');",
                "ALTER TABLE test ADD c0 bigint;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added same column c0');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('add-column', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name::text FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", TEXT)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                            &[T("c2")],
                            &[T("c3")],
                            &[T("c4")],
                            &[T("c5")],
                            &[T("c0")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches add different column. merge. no conflict",
            intent: "Different columns added on each side both appear in the merged table.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test (pk, c1, c2, c3, c4, c5) VALUES (0, 1, 2, 3, 4, 5);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('add-column');",
                "ALTER TABLE test ADD c0 bigint;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added column c0');",
                "SELECT * FROM dolt_checkout('add-column');",
                "ALTER TABLE test ADD c6 bigint;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added column c6');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('add-column', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8), Column("c0", INT8), Column("c6", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches add same column, different types. merge. conflict",
            intent: "The same column added with different types on each side is a conflict.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('add-column');",
                "ALTER TABLE test ADD c0 text;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added column c0 as string');",
                "SELECT * FROM dolt_checkout('add-column');",
                "ALTER TABLE test ADD c0 bigint;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added column c0 as int');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('add-column', '-m', 'merge');",
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
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches delete same column. merge. no conflict",
            intent: "The same column dropped on both sides merges.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('delete-column');",
                "ALTER TABLE test DROP COLUMN c5;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'deleted c5 column');",
                "SELECT * FROM dolt_checkout('delete-column');",
                "ALTER TABLE test DROP COLUMN c5;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'deleted c5 again');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('delete-column', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name::text FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", TEXT)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                            &[T("c2")],
                            &[T("c3")],
                            &[T("c4")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches delete different column. merge. no conflict",
            intent: "Different columns dropped on each side are both gone from the merged table.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT * FROM dolt_checkout('-b', 'one');",
                "ALTER TABLE test DROP COLUMN c5;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'deleted column c5');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('-b', 'two');",
                "ALTER TABLE test DROP COLUMN c4;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'deleted column c4');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('one', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name::text FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", TEXT)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                            &[T("c2")],
                            &[T("c3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches rename same column to same name. merge. no conflict",
            intent: "The same column renamed to the same name on both sides merges.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('rename-column');",
                "ALTER TABLE test RENAME COLUMN c5 TO c0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'renamed c5 to c0');",
                "SELECT * FROM dolt_checkout('rename-column');",
                "ALTER TABLE test RENAME COLUMN c5 TO c0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'renamed c5 to c0 again');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('rename-column', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name::text FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", TEXT)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                            &[T("c2")],
                            &[T("c3")],
                            &[T("c4")],
                            &[T("c0")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches rename same column to different name. merge. conflict",
            intent: "One column renamed to different names on each side is a conflict.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('rename-column');",
                "ALTER TABLE test RENAME COLUMN c5 TO c0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'renamed c5 to c0');",
                "SELECT * FROM dolt_checkout('rename-column');",
                "ALTER TABLE test RENAME COLUMN c5 TO c6;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'renamed c5 to c6');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('rename-column', '-m', 'merge');",
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
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: two branches rename different column to same name. merge. conflict",
            intent: "Different columns renamed to the same name on each side are a conflict.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('rename-column');",
                "ALTER TABLE test RENAME COLUMN c5 TO c0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'renamed c5 to c0');",
                "SELECT * FROM dolt_checkout('rename-column');",
                "ALTER TABLE test RENAME COLUMN c4 TO c0;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'renamed c5 to c6');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('rename-column', '-m', 'merge');",
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
            ],
            ..S
        },
        ScriptTest {
            name: "conflict-detection: adding primary key also adds not null constraint. merge. no conflict",
            intent: "Making a primary key column NOT NULL changes nothing, so merging that branch finds nothing to merge.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE test ALTER COLUMN pk TYPE int, ALTER COLUMN pk SET NOT NULL, ALTER COLUMN pk DROP DEFAULT;",
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
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("Everything up-to-date")],
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
