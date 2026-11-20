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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_schema_merge.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_schema_change_tests_basic_cases() {
    run_scripts(&[
        ScriptTest {
            name: "dropping columns: right to left merges",
            intent: "A column dropped on one side is dropped from the merged table while the other side's row changes merge.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100)); CREATE UNIQUE INDEX unique1 ON t (col2, pk);",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (pk, col1, col2);",
                "CREATE INDEX idx3 ON t (col1, col2);",
                "CREATE INDEX idx4 ON t (pk, col2);",
                "CREATE INDEX idx5 ON t(col2(2));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col1;",
                "INSERT INTO t VALUES (3, '300'), (4, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("200")],
                            &[T("3"), T("300")],
                            &[T("4"), T("400")],
                            &[T("5"), T("500")],
                            &[T("6"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "dropping columns: left to right merges",
            intent: "A column dropped on one side is dropped from the merged table while the other side's row changes merge.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100)); CREATE UNIQUE INDEX unique1 ON t (col2, pk);",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (pk, col1, col2);",
                "CREATE INDEX idx3 ON t (col1, col2);",
                "CREATE INDEX idx4 ON t (pk, col2);",
                "CREATE INDEX idx5 ON t(col2(2));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col1;",
                "INSERT INTO t VALUES (3, '300'), (4, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("200")],
                            &[T("3"), T("300")],
                            &[T("4"), T("400")],
                            &[T("5"), T("500")],
                            &[T("6"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "renaming a column: right to left merges",
            intent: "A column renamed on one side keeps its values and the other side's changes to them.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (col1, pk);",
                "CREATE INDEX idx3 ON t (pk, col1, col2);",
                "CREATE INDEX idx4 ON t (col1, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t RENAME COLUMN col1 TO col11;",
                "INSERT INTO t VALUES (3, 30, '300'), (4, 40, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col11", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "renaming a column: left to right merges",
            intent: "A column renamed on one side keeps its values and the other side's changes to them.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (col1, pk);",
                "CREATE INDEX idx3 ON t (pk, col1, col2);",
                "CREATE INDEX idx4 ON t (col1, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t RENAME COLUMN col1 TO col11;",
                "INSERT INTO t VALUES (3, 30, '300'), (4, 40, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col11", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "renaming and reordering a column: right to left merges",
            intent: "A column renamed and moved on one side merges by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (col2);",
                "CREATE INDEX idx3 ON t (pk, col1, col2);",
                "CREATE INDEX idx4 ON t (col1, col2);",
                "CREATE INDEX idx5 ON t (col2, col1);",
                "CREATE INDEX idx6 ON t (col2, pk, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t RENAME COLUMN col1 TO col11;",
                "ALTER TABLE t ALTER COLUMN col11 TYPE int after col2, ALTER COLUMN col11 DROP NOT NULL, ALTER COLUMN col11 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, '300', 30), (4, '400', 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col11, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col11", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't move a column to another position"),
            ..S
        },
        ScriptTest {
            name: "renaming and reordering a column: left to right merges",
            intent: "A column renamed and moved on one side merges by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (col2);",
                "CREATE INDEX idx3 ON t (pk, col1, col2);",
                "CREATE INDEX idx4 ON t (col1, col2);",
                "CREATE INDEX idx5 ON t (col2, col1);",
                "CREATE INDEX idx6 ON t (col2, pk, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t RENAME COLUMN col1 TO col11;",
                "ALTER TABLE t ALTER COLUMN col11 TYPE int after col2, ALTER COLUMN col11 DROP NOT NULL, ALTER COLUMN col11 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, '300', 30), (4, '400', 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col11, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col11", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't move a column to another position"),
            ..S
        },
        ScriptTest {
            name: "reordering a column: right to left merges",
            intent: "A column moved to another position on one side merges by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (col2);",
                "CREATE INDEX idx3 ON t (pk, col1, col2);",
                "CREATE INDEX idx4 ON t (col1, col2);",
                "CREATE INDEX idx5 ON t (col2, col1);",
                "CREATE INDEX idx6 ON t (col2, pk, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int after col2, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t (pk, col1, col2) VALUES (3, 30, '300'), (4, 40, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't move a column to another position"),
            ..S
        },
        ScriptTest {
            name: "reordering a column: left to right merges",
            intent: "A column moved to another position on one side merges by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (col2);",
                "CREATE INDEX idx3 ON t (pk, col1, col2);",
                "CREATE INDEX idx4 ON t (col1, col2);",
                "CREATE INDEX idx5 ON t (col2, col1);",
                "CREATE INDEX idx6 ON t (col2, pk, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int after col2, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t (pk, col1, col2) VALUES (3, 30, '300'), (4, 40, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't move a column to another position"),
            ..S
        },
        ScriptTest {
            name: "adding nullable columns to one side: right to left merges",
            intent: "Nullable columns added on one side are NULL in the other side's new rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "CREATE INDEX idx1 ON t (col1, pk);",
                "CREATE INDEX idx2 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "INSERT INTO t VALUES (2, 2, 2, 2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null],
                            &[T("2"), T("2"), T("2"), T("2")],
                            &[T("3"), T("3"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding nullable columns to one side: left to right merges",
            intent: "Nullable columns added on one side are NULL in the other side's new rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "CREATE INDEX idx1 ON t (col1, pk);",
                "CREATE INDEX idx2 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "INSERT INTO t VALUES (2, 2, 2, 2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null],
                            &[T("2"), T("2"), T("2"), T("2")],
                            &[T("3"), T("3"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a column with a literal default value: right to left merges",
            intent: "A column added with a literal default fills the other side's new rows with it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN c1 varchar(100) DEFAULT ('hello');",
                "INSERT INTO t VALUES (2, 'hi');",
                "CREATE INDEX idx1 ON t (c1, pk);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("hello")],
                            &[T("2"), T("hi")],
                            &[T("3"), T("hello")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a column with a literal default value: left to right merges",
            intent: "A column added with a literal default fills the other side's new rows with it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c1 varchar(100) DEFAULT ('hello');",
                "INSERT INTO t VALUES (2, 'hi');",
                "CREATE INDEX idx1 ON t (c1, pk);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("hello")],
                            &[T("2"), T("hi")],
                            &[T("3"), T("hello")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "altering a column to add a literal default value: right to left merges",
            intent: "A default added to a column on one side merges with the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "INSERT INTO t VALUES (1, NULL);",
                "CREATE INDEX idx1 ON t (c1, pk);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN c1 TYPE varchar(100), ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 SET DEFAULT ('hello');",
                "INSERT INTO t VALUES (2, DEFAULT);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, NULL);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("hello")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "altering a column to add a literal default value: left to right merges",
            intent: "A default added to a column on one side merges with the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "INSERT INTO t VALUES (1, NULL);",
                "CREATE INDEX idx1 ON t (c1, pk);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN c1 TYPE varchar(100), ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 SET DEFAULT ('hello');",
                "INSERT INTO t VALUES (2, DEFAULT);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, NULL);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("hello")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a column with a non-literal default value: right to left merges",
            intent: "A column added with an expression default fills the other side's new rows with it.",
            set_up_script: &[
                "CREATE TABLE t (pk varchar(100) PRIMARY KEY);",
                "INSERT INTO t VALUES ('1');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN c1 varchar(100) DEFAULT (concat('h', 'e', 'l', 'l', 'o'));",
                "INSERT INTO t VALUES ('2', 'hi');",
                "CREATE INDEX idx1 ON t (c1, pk);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES ('3');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("hello")],
                            &[T("2"), T("hi")],
                            &[T("3"), T("hello")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("a Postgres DEFAULT can't read other columns, so the added column's default is a constant expression")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a column with a non-literal default value: left to right merges",
            intent: "A column added with an expression default fills the other side's new rows with it.",
            set_up_script: &[
                "CREATE TABLE t (pk varchar(100) PRIMARY KEY);",
                "INSERT INTO t VALUES ('1');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c1 varchar(100) DEFAULT (concat('h', 'e', 'l', 'l', 'o'));",
                "INSERT INTO t VALUES ('2', 'hi');",
                "CREATE INDEX idx1 ON t (c1, pk);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES ('3');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("hello")],
                            &[T("2"), T("hi")],
                            &[T("3"), T("hello")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("a Postgres DEFAULT can't read other columns, so the added column's default is a constant expression")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "right-side adds a column with a default value, left-side drops a column: right to left merges",
            intent: "A column added with a default on one side and another dropped on the other both apply.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100), c2 varchar(100));",
                "INSERT INTO t VALUES ('1', 'BAD', 'hello');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN c3 varchar(100) DEFAULT (concat('hello', 'h', 'e', 'l', 'l', 'o'));",
                "INSERT INTO t VALUES ('2', 'BAD', 'hello', 'hi');",
                "CREATE INDEX idx1 ON t (c1, pk);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES ('3', 'BAD', 'hello');",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR), Column("c3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("hello"), T("hellohello")],
                            &[T("2"), T("hello"), T("hi")],
                            &[T("3"), T("hello"), T("hellohello")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "right-side adds a column with a default value, left-side drops a column: left to right merges",
            intent: "A column added with a default on one side and another dropped on the other both apply.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100), c2 varchar(100));",
                "INSERT INTO t VALUES ('1', 'BAD', 'hello');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c3 varchar(100) DEFAULT (concat('hello', 'h', 'e', 'l', 'l', 'o'));",
                "INSERT INTO t VALUES ('2', 'BAD', 'hello', 'hi');",
                "CREATE INDEX idx1 ON t (c1, pk);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES ('3', 'BAD', 'hello');",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR), Column("c3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("hello"), T("hellohello")],
                            &[T("2"), T("hello"), T("hi")],
                            &[T("3"), T("hello"), T("hellohello")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "right-side adds a column with a default value: right to left merges",
            intent: "A column added with a default on one side fills the other side's new rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100), c2 varchar(100));",
                "INSERT INTO t VALUES ('1', 'BAD', 'hello');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN c3 varchar(100) DEFAULT (concat('hello', 'BAD', 'default'));",
                "INSERT INTO t VALUES ('2', 'BAD', 'hello', 'hi');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES ('3', 'BAD', 'hi');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR), Column("c3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("BAD"), T("hello"), T("helloBADdefault")],
                            &[T("2"), T("BAD"), T("hello"), T("hi")],
                            &[T("3"), T("BAD"), T("hi"), T("helloBADdefault")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("a Postgres DEFAULT can't read other columns, so the added column's default is a constant expression")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "right-side adds a column with a default value: left to right merges",
            intent: "A column added with a default on one side fills the other side's new rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100), c2 varchar(100));",
                "INSERT INTO t VALUES ('1', 'BAD', 'hello');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c3 varchar(100) DEFAULT (concat('hello', 'BAD', 'default'));",
                "INSERT INTO t VALUES ('2', 'BAD', 'hello', 'hi');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES ('3', 'BAD', 'hi');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR), Column("c3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("BAD"), T("hello"), T("helloBADdefault")],
                            &[T("2"), T("BAD"), T("hello"), T("hi")],
                            &[T("3"), T("BAD"), T("hi"), T("helloBADdefault")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("a Postgres DEFAULT can't read other columns, so the added column's default is a constant expression")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding different columns to both sides: right to left merges",
            intent: "Different columns added on each side are both in the merged table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "CREATE INDEX idx1 ON t (pk);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100);",
                "INSERT INTO t VALUES (3, '300'), (4, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null, Null],
                            &[T("2"), Null, Null],
                            &[T("3"), Null, T("300")],
                            &[T("4"), Null, T("400")],
                            &[T("5"), T("50"), Null],
                            &[T("6"), T("60"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding different columns to both sides: left to right merges",
            intent: "Different columns added on each side are both in the merged table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "CREATE INDEX idx1 ON t (pk);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100);",
                "INSERT INTO t VALUES (3, '300'), (4, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null, Null],
                            &[T("2"), Null, Null],
                            &[T("3"), Null, T("300")],
                            &[T("4"), Null, T("400")],
                            &[T("5"), T("50"), Null],
                            &[T("6"), T("60"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding columns with default values to both sides: right to left merges",
            intent: "Columns with defaults added on each side both fill the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "CREATE INDEX idx1 ON t (pk);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100) DEFAULT 'abc';",
                "INSERT INTO t VALUES (3, '300'), (4, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col1 int DEFAULT 101;",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("101"), T("abc")],
                            &[T("2"), T("101"), T("abc")],
                            &[T("3"), T("101"), T("300")],
                            &[T("4"), T("101"), T("400")],
                            &[T("5"), T("50"), T("abc")],
                            &[T("6"), T("60"), T("abc")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding columns with default values to both sides: left to right merges",
            intent: "Columns with defaults added on each side both fill the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "CREATE INDEX idx1 ON t (pk);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100) DEFAULT 'abc';",
                "INSERT INTO t VALUES (3, '300'), (4, '400');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col1 int DEFAULT 101;",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("101"), T("abc")],
                            &[T("2"), T("101"), T("abc")],
                            &[T("3"), T("101"), T("300")],
                            &[T("4"), T("101"), T("400")],
                            &[T("5"), T("50"), T("abc")],
                            &[T("6"), T("60"), T("abc")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding indexed columns to both sides: right to left merges",
            intent: "Indexed columns added on each side keep both indexes in the merged table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100);",
                "INSERT INTO t (pk, col2) VALUES (3, '3hello'), (4, '4hello');",
                "CREATE INDEX ON t (col2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col1 int DEFAULT 100;",
                "INSERT INTO t (pk) VALUES (5), (6);",
                "CREATE INDEX ON t (col1);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("100"), Null],
                            &[T("2"), T("100"), Null],
                            &[T("3"), T("100"), T("3hello")],
                            &[T("4"), T("100"), T("4hello")],
                            &[T("5"), T("100"), Null],
                            &[T("6"), T("100"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("a Postgres DEFAULT can't read other columns, so the added column's default is a constant expression")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding indexed columns to both sides: left to right merges",
            intent: "Indexed columns added on each side keep both indexes in the merged table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100);",
                "INSERT INTO t (pk, col2) VALUES (3, '3hello'), (4, '4hello');",
                "CREATE INDEX ON t (col2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col1 int DEFAULT 100;",
                "INSERT INTO t (pk) VALUES (5), (6);",
                "CREATE INDEX ON t (col1);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("100"), Null],
                            &[T("2"), T("100"), Null],
                            &[T("3"), T("100"), T("3hello")],
                            &[T("4"), T("100"), T("4hello")],
                            &[T("5"), T("100"), Null],
                            &[T("6"), T("100"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("a Postgres DEFAULT can't read other columns, so the added column's default is a constant expression")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping and adding a column with the same name: right to left merges",
            intent: "A column dropped and added again under its name is a new column, not the old one.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (col1, pk);",
                "CREATE INDEX idx2 ON t (col2, pk);",
                "CREATE INDEX idx3 ON t (col2, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col1;",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "INSERT INTO t VALUES (3, '300', 30), (4, '400', 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null, T("100")],
                            &[T("2"), Null, T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping and adding a column with the same name: left to right merges",
            intent: "A column dropped and added again under its name is a new column, not the old one.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (col1, pk);",
                "CREATE INDEX idx2 ON t (col2, pk);",
                "CREATE INDEX idx3 ON t (col2, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col1;",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "INSERT INTO t VALUES (3, '300', 30), (4, '400', 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null, T("100")],
                            &[T("2"), Null, T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop a column, schema contains BLOB/AddrEnc columns: right to left merges",
            intent: "Dropping a column beside out-of-line values keeps the other columns' values.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 text, col3 varchar(10));",
                "INSERT INTO t VALUES (1, 10, 'a', 'b'), (2, 20, 'c', 'd');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (300, 30, 'e', 'f');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col1;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col2", TEXT), Column("col3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a"), T("b")],
                            &[T("2"), T("c"), T("d")],
                            &[T("300"), T("e"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop a column, schema contains BLOB/AddrEnc columns: left to right merges",
            intent: "Dropping a column beside out-of-line values keeps the other columns' values.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 text, col3 varchar(10));",
                "INSERT INTO t VALUES (1, 10, 'a', 'b'), (2, 20, 'c', 'd');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (300, 30, 'e', 'f');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col1;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col2", TEXT), Column("col3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a"), T("b")],
                            &[T("2"), T("c"), T("d")],
                            &[T("300"), T("e"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "convergent schema changes: right to left merges",
            intent: "The same schema changes made on both sides merge into one.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE parent(id int PRIMARY KEY);",
                "INSERT INTO parent VALUES (1), (2), (3);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ADD COLUMN col3 int ;",
                "CREATE INDEX idx1 ON t (col3, col1);",
                "ALTER TABLE t ADD CONSTRAINT fk1 FOREIGN KEY (col3) REFERENCES parent(id);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "CREATE INDEX idx1 ON t (col3, col1);",
                "UPDATE t SET col1=-1000 WHERE t.pk = 1;",
                "ALTER TABLE t ADD CONSTRAINT fk1 FOREIGN KEY (col3) REFERENCES parent(id);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int NOT NULL,\n  `col3` int,\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col3`,`col1`),\n  CONSTRAINT `fk1` FOREIGN KEY (`col3`) REFERENCES `parent` (`id`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("-1000"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "convergent schema changes: left to right merges",
            intent: "The same schema changes made on both sides merge into one.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE parent(id int PRIMARY KEY);",
                "INSERT INTO parent VALUES (1), (2), (3);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ADD COLUMN col3 int ;",
                "CREATE INDEX idx1 ON t (col3, col1);",
                "ALTER TABLE t ADD CONSTRAINT fk1 FOREIGN KEY (col3) REFERENCES parent(id);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "CREATE INDEX idx1 ON t (col3, col1);",
                "UPDATE t SET col1=-1000 WHERE t.pk = 1;",
                "ALTER TABLE t ADD CONSTRAINT fk1 FOREIGN KEY (col3) REFERENCES parent(id);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int NOT NULL,\n  `col3` int,\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col3`,`col1`),\n  CONSTRAINT `fk1` FOREIGN KEY (`col3`) REFERENCES `parent` (`id`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("-1000"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ambiguous choice of ancestor column: right to left merges",
            intent: "A column dropped and another added on one side is matched by column identity, not by name or position.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int);",
                "INSERT INTO t VALUES (1, 10, 100), (2, 20, 200);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col2;",
                "INSERT INTO t VALUES (3, 30), (4, 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col1;",
                "ALTER TABLE t RENAME COLUMN col2 TO col1;",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Bug("Dolt's summary shows nothing for a merge that stops with a schema conflict, where the summary reports the conflict the merge records")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ambiguous choice of ancestor column: left to right merges",
            intent: "A column dropped and another added on one side is matched by column identity, not by name or position.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int);",
                "INSERT INTO t VALUES (1, 10, 100), (2, 20, 200);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col2;",
                "INSERT INTO t VALUES (3, 30), (4, 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col1;",
                "ALTER TABLE t RENAME COLUMN col2 TO col1;",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Bug("Dolt's summary shows nothing for a merge that stops with a schema conflict, where the summary reports the conflict the merge records")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "creating new column to replace ancestor column: right to left merges",
            intent: "A new column replacing a dropped one is matched by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 30), (4, 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "UPDATE t SET col2 = 10*col1;",
                "ALTER TABLE t DROP COLUMN col1;",
                "ALTER TABLE t RENAME COLUMN col2 TO col1;",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("200")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("50")],
                            &[T("6"), T("60")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("columns are matched by column id, so a column added in place of one the other side still has is a new column and their values of the old one go with its drop (MERGE_PLAN §4), where Dolt matches it by name")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "creating new column to replace ancestor column: left to right merges",
            intent: "A new column replacing a dropped one is matched by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 30), (4, 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "UPDATE t SET col2 = 10*col1;",
                "ALTER TABLE t DROP COLUMN col1;",
                "ALTER TABLE t RENAME COLUMN col2 TO col1;",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("200")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("50")],
                            &[T("6"), T("60")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("columns are matched by column id, so a column added in place of one the other side still has is a new column and their values of the old one go with its drop (MERGE_PLAN §4), where Dolt matches it by name")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: ": right to left merges",
            intent: "Rows added on both sides to a table with a descending and an ascending index on one column merge.",
            set_up_script: &[
                "CREATE TABLE t (id CHAR(36) PRIMARY KEY, time timestamp); CREATE INDEX i1 ON t (time DESC); CREATE INDEX i2 ON t (time);",
                "INSERT INTO t VALUES (gen_random_uuid()::text, now());",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (gen_random_uuid()::text, now());",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (gen_random_uuid()::text, now());",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: ": left to right merges",
            intent: "Rows added on both sides to a table with a descending and an ascending index on one column merge.",
            set_up_script: &[
                "CREATE TABLE t (id CHAR(36) PRIMARY KEY, time timestamp); CREATE INDEX i1 ON t (time DESC); CREATE INDEX i2 ON t (time);",
                "INSERT INTO t VALUES (gen_random_uuid()::text, now());",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (gen_random_uuid()::text, now());",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (gen_random_uuid()::text, now());",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "creating new column to replace ancestor column: right to left merges",
            intent: "A new column replacing a dropped one is matched by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD new_col text DEFAULT NULL;",
                "INSERT INTO t VALUES (3, 'three'), (4, 'four');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5), (6);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("new_col", TEXT)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("three")],
                            &[T("4"), T("four")],
                            &[T("5"), Null],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "creating new column to replace ancestor column: left to right merges",
            intent: "A new column replacing a dropped one is matched by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD new_col text DEFAULT NULL;",
                "INSERT INTO t VALUES (3, 'three'), (4, 'four');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5), (6);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("new_col", TEXT)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("three")],
                            &[T("4"), T("four")],
                            &[T("5"), Null],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_schema_change_tests_for_data_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "data conflict: right to left merges",
            intent: "A row changed on both sides conflicts across a schema change, and the conflict table shows each side's columns.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100), col3 varchar(50), col4 varchar(20)); CREATE UNIQUE INDEX unique1 ON t (col2, pk);",
                "INSERT INTO t VALUES (1, 10, '100', '1', '11'), (2, 20, '200', '2', '22');",
                "CREATE INDEX idx1 ON t (col4, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col3;",
                "UPDATE t SET col1=-100, col2='-100' WHERE pk = 1;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1=-1000 WHERE t.pk = 1;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, base_col2, base_col3, base_col4, our_pk, our_col1, our_col2, our_col4, their_pk, their_col1, their_col2, their_col4 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("base_col2", VARCHAR), Column("base_col3", VARCHAR), Column("base_col4", VARCHAR), Column("our_pk", INT4), Column("our_col1", INT4), Column("our_col2", VARCHAR), Column("our_col4", VARCHAR), Column("their_pk", INT4), Column("their_col1", INT4), Column("their_col2", VARCHAR), Column("their_col4", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("100"), T("1"), T("11"), T("1"), T("-1000"), T("100"), T("11"), T("1"), T("-100"), T("-100"), T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_col1 = their_col1, their_col2 = our_col2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR), Column("col4", VARCHAR)],
                        rows: &[
                            &[T("1"), T("-100"), T("100"), T("11")],
                            &[T("2"), T("20"), T("200"), T("22")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "data conflict: left to right merges",
            intent: "A row changed on both sides conflicts across a schema change, and the conflict table shows each side's columns.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100), col3 varchar(50), col4 varchar(20)); CREATE UNIQUE INDEX unique1 ON t (col2, pk);",
                "INSERT INTO t VALUES (1, 10, '100', '1', '11'), (2, 20, '200', '2', '22');",
                "CREATE INDEX idx1 ON t (col4, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col3;",
                "UPDATE t SET col1=-100, col2='-100' WHERE pk = 1;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col1=-1000 WHERE t.pk = 1;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, base_col2, base_col3, base_col4, their_pk, their_col1, their_col2, their_col4, our_pk, our_col1, our_col2, our_col4 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("base_col2", VARCHAR), Column("base_col3", VARCHAR), Column("base_col4", VARCHAR), Column("their_pk", INT4), Column("their_col1", INT4), Column("their_col2", VARCHAR), Column("their_col4", VARCHAR), Column("our_pk", INT4), Column("our_col1", INT4), Column("our_col2", VARCHAR), Column("our_col4", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("100"), T("1"), T("11"), T("1"), T("-1000"), T("100"), T("11"), T("1"), T("-100"), T("-100"), T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET their_col1 = our_col1, our_col2 = their_col2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR), Column("col4", VARCHAR)],
                        rows: &[
                            &[T("1"), T("-100"), T("100"), T("11")],
                            &[T("2"), T("20"), T("200"), T("22")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_schema_change_tests_collations() {
    run_scripts(&[
        ScriptTest {
            name: "Changing a table's default collation on one side: right to left merges",
            intent: "A table's default collation changed on one side merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100)) ;",
                "INSERT INTO t VALUES (1, '10');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (2, '20');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '30');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100) CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci,\n  PRIMARY KEY (`pk`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a table's default collation is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "Changing a table's default collation on one side: left to right merges",
            intent: "A table's default collation changed on one side merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100)) ;",
                "INSERT INTO t VALUES (1, '10');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (2, '20');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, '30');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100) CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci,\n  PRIMARY KEY (`pk`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a table's default collation is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "Changing a table's default collation on both sides to different values: right to left merges",
            intent: "A table's default collation changed differently on both sides fails the merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100)) ;",
                "INSERT INTO t VALUES (1, '10');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (2, '20');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (3, '30');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge.ErrDefaultCollationConflict", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge.ErrDefaultCollationConflict", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a table's default collation is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "Changing a table's default collation on both sides to different values: left to right merges",
            intent: "A table's default collation changed differently on both sides fails the merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100)) ;",
                "INSERT INTO t VALUES (1, '10');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (2, '20');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (3, '30');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge.ErrDefaultCollationConflict", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge.ErrDefaultCollationConflict", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a table's default collation is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "Changing a table's default collation on both sides to the same value: right to left merges",
            intent: "A table's default collation changed the same way on both sides merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100)) ;",
                "INSERT INTO t VALUES (1, '10');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (2, '20');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (3, '30');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100) CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci,\n  PRIMARY KEY (`pk`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a table's default collation is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "Changing a table's default collation on both sides to the same value: left to right merges",
            intent: "A table's default collation changed the same way on both sides merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100)) ;",
                "INSERT INTO t VALUES (1, '10');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (2, '20');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ;",
                "INSERT INTO t VALUES (3, '30');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100) CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci,\n  PRIMARY KEY (`pk`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a table's default collation is MySQL's"),
            ..S
        },
    ]);
}

#[test]
fn test_schema_change_tests_constraints() {
    run_scripts(&[
        ScriptTest {
            name: "updating fk index when ancestor schema has changed: right to left merges",
            intent: "A foreign key's index follows a schema change made on the other side.",
            set_up_script: &[
                "CREATE TABLE parent(pk int PRIMARY KEY, c1 varchar(100));",
                "CREATE TABLE child(pk int PRIMARY KEY, remove_me int, parent_id int, CONSTRAINT fk1 FOREIGN KEY (parent_id) REFERENCES parent(pk)); CREATE INDEX \"fk_idx1\" ON child (\"parent_id\");",
                "INSERT INTO parent VALUES (100, 'one hundred'), (200, 'two hundred');",
                "INSERT INTO child VALUES (1, -1, 100), (2, -1, 200);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "DELETE FROM child;",
                "DELETE FROM parent;",
                "ALTER TABLE child DROP COLUMN remove_me;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE child DROP COLUMN remove_me;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "updating fk index when ancestor schema has changed: left to right merges",
            intent: "A foreign key's index follows a schema change made on the other side.",
            set_up_script: &[
                "CREATE TABLE parent(pk int PRIMARY KEY, c1 varchar(100));",
                "CREATE TABLE child(pk int PRIMARY KEY, remove_me int, parent_id int, CONSTRAINT fk1 FOREIGN KEY (parent_id) REFERENCES parent(pk)); CREATE INDEX \"fk_idx1\" ON child (\"parent_id\");",
                "INSERT INTO parent VALUES (100, 'one hundred'), (200, 'two hundred');",
                "INSERT INTO child VALUES (1, -1, 100), (2, -1, 200);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "DELETE FROM child;",
                "DELETE FROM parent;",
                "ALTER TABLE child DROP COLUMN remove_me;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE child DROP COLUMN remove_me;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "removing a not-null constraint: right to left merges",
            intent: "A NOT NULL removed on one side lets the other side's rows merge.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int NOT NULL);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "CREATE INDEX idx1 ON t (col1, pk);",
                "CREATE INDEX idx2 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, NULL);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "removing a not-null constraint: left to right merges",
            intent: "A NOT NULL removed on one side lets the other side's rows merge.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int NOT NULL);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "CREATE INDEX idx1 ON t (col1, pk);",
                "CREATE INDEX idx2 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, NULL);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a foreign key to one side, with fk constraint violation: right to left merges",
            intent: "A foreign key added on one side records violations for the other side's rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (pk int PRIMARY KEY, p_fk int);",
                "INSERT INTO parent VALUES (1);",
                "INSERT INTO child VALUES (1, 1);",
                "SET dolt_force_transaction_commit = TRUE;",
                "CREATE INDEX idx1 ON child (p_fk, pk);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE child ADD CONSTRAINT fk_parent FOREIGN KEY (p_fk) REFERENCES parent(pk);",
                "ALTER TABLE child ADD COLUMN col1 int;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO child VALUES (2, 2);",
                "UPDATE child SET p_fk = 3 WHERE pk = 1;",
                "ALTER TABLE child ADD COLUMN col2 varchar(100);",
                "UPDATE child SET col2 = '1col2' WHERE pk = 1;",
                "UPDATE child SET col2 = '2col2' WHERE pk = 2;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, p_fk, col1, col2 FROM child ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("p_fk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("3"), Null, T("1col2")],
                            &[T("2"), T("2"), Null, T("2col2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, p_fk, col1, col2 FROM dolt_constraint_violations_child ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("p_fk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("3"), Null, T("1col2")],
                            &[T("2"), T("2"), Null, T("2col2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a foreign key to one side, with fk constraint violation: left to right merges",
            intent: "A foreign key added on one side records violations for the other side's rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (pk int PRIMARY KEY, p_fk int);",
                "INSERT INTO parent VALUES (1);",
                "INSERT INTO child VALUES (1, 1);",
                "SET dolt_force_transaction_commit = TRUE;",
                "CREATE INDEX idx1 ON child (p_fk, pk);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE child ADD CONSTRAINT fk_parent FOREIGN KEY (p_fk) REFERENCES parent(pk);",
                "ALTER TABLE child ADD COLUMN col1 int;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO child VALUES (2, 2);",
                "UPDATE child SET p_fk = 3 WHERE pk = 1;",
                "ALTER TABLE child ADD COLUMN col2 varchar(100);",
                "UPDATE child SET col2 = '1col2' WHERE pk = 1;",
                "UPDATE child SET col2 = '2col2' WHERE pk = 2;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, p_fk, col1, col2 FROM child ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("p_fk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("3"), Null, T("1col2")],
                            &[T("2"), T("2"), Null, T("2col2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, p_fk, col1, col2 FROM dolt_constraint_violations_child ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("p_fk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("3"), Null, T("1col2")],
                            &[T("2"), T("2"), Null, T("2col2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraints on mixed-case columns with divergent data and schema changes: right to left merges",
            intent: "Check constraints on mixed-case columns keep working after a merge with schema changes.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id varchar(8) PRIMARY KEY, accelClockEnabled int NOT NULL DEFAULT 0, CONSTRAINT chk_accel CHECK (accelClockEnabled <= 255));",
                "INSERT INTO t VALUES ('r1', 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN coachModeEnabled int NOT NULL DEFAULT 0;",
                "ALTER TABLE t ADD COLUMN mascotModeType varchar(255);",
                "ALTER TABLE t ADD CONSTRAINT chk_coach CHECK (COACHMODEENABLED <= 255);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET accelClockEnabled = 2 WHERE id = 'r1';",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR), Column("accelclockenabled", INT4), Column("coachmodeenabled", INT4), Column("mascotmodetype", VARCHAR)],
                        rows: &[
                            &[T("r1"), T("2"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET accelClockEnabled = 256 WHERE id = 'r1';",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t\" violates check constraint \"chk_accel\"", detail: "Failing row contains (r1, 256, 0, null).", schema: "public", table: "t", constraint: "chk_accel", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET coachModeEnabled = 256 WHERE id = 'r1';",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t\" violates check constraint \"chk_coach\"", detail: "Failing row contains (r1, 2, 256, null).", schema: "public", table: "t", constraint: "chk_coach", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR), Column("accelclockenabled", INT4), Column("coachmodeenabled", INT4), Column("mascotmodetype", VARCHAR)],
                        rows: &[
                            &[T("r1"), T("2"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraints on mixed-case columns with divergent data and schema changes: left to right merges",
            intent: "Check constraints on mixed-case columns keep working after a merge with schema changes.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id varchar(8) PRIMARY KEY, accelClockEnabled int NOT NULL DEFAULT 0, CONSTRAINT chk_accel CHECK (accelClockEnabled <= 255));",
                "INSERT INTO t VALUES ('r1', 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN coachModeEnabled int NOT NULL DEFAULT 0;",
                "ALTER TABLE t ADD COLUMN mascotModeType varchar(255);",
                "ALTER TABLE t ADD CONSTRAINT chk_coach CHECK (COACHMODEENABLED <= 255);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET accelClockEnabled = 2 WHERE id = 'r1';",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR), Column("accelclockenabled", INT4), Column("coachmodeenabled", INT4), Column("mascotmodetype", VARCHAR)],
                        rows: &[
                            &[T("r1"), T("2"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET accelClockEnabled = 256 WHERE id = 'r1';",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t\" violates check constraint \"chk_accel\"", detail: "Failing row contains (r1, 256, 0, null).", schema: "public", table: "t", constraint: "chk_accel", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET coachModeEnabled = 256 WHERE id = 'r1';",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t\" violates check constraint \"chk_coach\"", detail: "Failing row contains (r1, 2, 256, null).", schema: "public", table: "t", constraint: "chk_coach", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR), Column("accelclockenabled", INT4), Column("coachmodeenabled", INT4), Column("mascotmodetype", VARCHAR)],
                        rows: &[
                            &[T("r1"), T("2"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint references a deleted mixed-case column: right to left merges",
            intent: "A check constraint on a column the other side deleted is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, coachModeEnabled int);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD CONSTRAINT chk_coach CHECK (coachModeEnabled <= 255);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN coachModeEnabled;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("check 'chk_coach' references a column that will be deleted after merge")],
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
            name: "check constraint references a deleted mixed-case column: left to right merges",
            intent: "A check constraint on a column the other side deleted is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, coachModeEnabled int);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CONSTRAINT chk_coach CHECK (coachModeEnabled <= 255);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN coachModeEnabled;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("check 'chk_coach' references a column that will be deleted after merge")],
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
            name: "altering a check constraint on one side: right to left merges",
            intent: "A check constraint changed on one side applies to the merged rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "INSERT INTO t VALUES (1, 'one');",
                "ALTER TABLE t ADD CONSTRAINT check1 CHECK (c1 IN ('one', 'two'));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP CONSTRAINT check1;",
                "ALTER TABLE t ADD CONSTRAINT check1 CHECK (c1 IN ('one', 'two', 'three'));",
                "INSERT INTO t VALUES (3, 'three');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                            &[T("3"), T("three")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "altering a check constraint on one side: left to right merges",
            intent: "A check constraint changed on one side applies to the merged rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "INSERT INTO t VALUES (1, 'one');",
                "ALTER TABLE t ADD CONSTRAINT check1 CHECK (c1 IN ('one', 'two'));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP CONSTRAINT check1;",
                "ALTER TABLE t ADD CONSTRAINT check1 CHECK (c1 IN ('one', 'two', 'three'));",
                "INSERT INTO t VALUES (3, 'three');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                            &[T("3"), T("three")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping a foreign key: right to left merges",
            intent: "A foreign key dropped on one side no longer checks the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (pk int PRIMARY KEY, p_fk int, CONSTRAINT parent_fk FOREIGN KEY (p_fk) REFERENCES parent (pk));",
                "INSERT INTO parent VALUES (1);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE child DROP CONSTRAINT parent_fk;",
                "DELETE FROM parent;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO child VALUES (2, 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM parent;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("p_fk", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping a foreign key: left to right merges",
            intent: "A foreign key dropped on one side no longer checks the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (pk int PRIMARY KEY, p_fk int, CONSTRAINT parent_fk FOREIGN KEY (p_fk) REFERENCES parent (pk));",
                "INSERT INTO parent VALUES (1);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE child DROP CONSTRAINT parent_fk;",
                "DELETE FROM parent;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO child VALUES (2, 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM parent;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("p_fk", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "foreign keys – drop tables with FK reference between them: right to left merges",
            intent: "Tables referencing each other dropped on one side merge.",
            set_up_script: &[
                "CREATE TABLE a (id int PRIMARY KEY, name varchar(256) NOT NULL);",
                "CREATE TABLE b (id int PRIMARY KEY, assetId int, CONSTRAINT file_assetid_foreign FOREIGN KEY (assetId) REFERENCES a(id)); CREATE INDEX file_assetid_foreign ON b (assetId);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "CREATE TABLE c (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "DROP TABLE a, b;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "foreign keys – drop tables with FK reference between them: left to right merges",
            intent: "Tables referencing each other dropped on one side merge.",
            set_up_script: &[
                "CREATE TABLE a (id int PRIMARY KEY, name varchar(256) NOT NULL);",
                "CREATE TABLE b (id int PRIMARY KEY, assetId int, CONSTRAINT file_assetid_foreign FOREIGN KEY (assetId) REFERENCES a(id)); CREATE INDEX file_assetid_foreign ON b (assetId);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE c (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "DROP TABLE a, b;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a unique key, with unique key violation: right to left merges",
            intent: "A unique key added on one side records violations for the other side's duplicates.",
            set_up_script: &[
                "CREATE TABLE t (pk int, col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "SET dolt_force_transaction_commit = 1;",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD UNIQUE (col1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 1);",
                "INSERT INTO t VALUES (3, 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a unique key, with unique key violation: left to right merges",
            intent: "A unique key added on one side records violations for the other side's duplicates.",
            set_up_script: &[
                "CREATE TABLE t (pk int, col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "SET dolt_force_transaction_commit = 1;",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD UNIQUE (col1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (2, 1);",
                "INSERT INTO t VALUES (3, 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unique constraint violation: right to left merges",
            intent: "Rows from both sides with one unique value record unique violations.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk varchar(100) PRIMARY KEY, col1 int, col2 varchar(100)); CREATE UNIQUE INDEX unique1 ON t (col2);",
                "INSERT INTO t VALUES ('0', 0, '');",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col1;",
                "INSERT INTO t (pk, col2) VALUES ('10', 'same');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES ('1', 10, 'same');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col2, dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", VARCHAR), Column("col2", VARCHAR), Column("dolt_violation_info", JSONB)],
                        rows: &[
                            &[T("unique index"), T("1"), T("same"), T("{\"Name\": \"unique1\", \"Columns\": [\"col2\"]}")],
                            &[T("unique index"), T("10"), T("same"), T("{\"Name\": \"unique1\", \"Columns\": [\"col2\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("0"), T("")],
                            &[T("1"), T("same")],
                            &[T("10"), T("same")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unique constraint violation: left to right merges",
            intent: "Rows from both sides with one unique value record unique violations.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk varchar(100) PRIMARY KEY, col1 int, col2 varchar(100)); CREATE UNIQUE INDEX unique1 ON t (col2);",
                "INSERT INTO t VALUES ('0', 0, '');",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col1;",
                "INSERT INTO t (pk, col2) VALUES ('10', 'same');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES ('1', 10, 'same');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col2, dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", VARCHAR), Column("col2", VARCHAR), Column("dolt_violation_info", JSONB)],
                        rows: &[
                            &[T("unique index"), T("1"), T("same"), T("{\"Name\": \"unique1\", \"Columns\": [\"col2\"]}")],
                            &[T("unique index"), T("10"), T("same"), T("{\"Name\": \"unique1\", \"Columns\": [\"col2\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("0"), T("")],
                            &[T("1"), T("same")],
                            &[T("10"), T("same")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping a unique key: right to left merges",
            intent: "A unique key dropped on one side lets duplicates from the other side merge.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP col1;",
                "ALTER TABLE t ADD col1 int;",
                "UPDATE t SET col1 = 1 WHERE pk = 1;",
                "INSERT INTO t VALUES (2, 1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping a unique key: left to right merges",
            intent: "A unique key dropped on one side lets duplicates from the other side merge.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP col1;",
                "ALTER TABLE t ADD col1 int;",
                "UPDATE t SET col1 = 1 WHERE pk = 1;",
                "INSERT INTO t VALUES (2, 1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint - non-contiguous primary key: right to left merges",
            intent: "A check constraint on a table whose key columns aren't first checks the merged rows.",
            set_up_script: &[
                "CREATE TABLE t (pk1 int, col1 int, pk2 varchar(100), CHECK (col1 IN (0, 1)), PRIMARY KEY (pk1, pk2));",
                "INSERT INTO t VALUES (1, 0, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100);",
                "INSERT INTO t VALUES (2, 1, 2, 'hello');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 0, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("col1", INT4), Column("pk2", VARCHAR), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("0"), T("1"), Null],
                            &[T("2"), T("1"), T("2"), T("hello")],
                            &[T("3"), T("0"), T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint - non-contiguous primary key: left to right merges",
            intent: "A check constraint on a table whose key columns aren't first checks the merged rows.",
            set_up_script: &[
                "CREATE TABLE t (pk1 int, col1 int, pk2 varchar(100), CHECK (col1 IN (0, 1)), PRIMARY KEY (pk1, pk2));",
                "INSERT INTO t VALUES (1, 0, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100);",
                "INSERT INTO t VALUES (2, 1, 2, 'hello');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 0, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("col1", INT4), Column("pk2", VARCHAR), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("0"), T("1"), Null],
                            &[T("2"), T("1"), T("2"), T("hello")],
                            &[T("3"), T("0"), T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - simple case, no schema changes: right to left merges",
            intent: "Rows from both sides that together break a check constraint record a check violation.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK (col1 != col2));",
                "INSERT INTO t VALUES (1, 2, 3);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col2=4;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1=4;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, col2, CAST(dolt_violation_info AS text) LIKE '%(col1 <> col2)%' FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("?column?", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("4"), T("4"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, a CHECK constraint's expression as Postgres prints it; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - simple case, no schema changes: left to right merges",
            intent: "Rows from both sides that together break a check constraint record a check violation.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK (col1 != col2));",
                "INSERT INTO t VALUES (1, 2, 3);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col2=4;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col1=4;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, col2, CAST(dolt_violation_info AS text) LIKE '%(col1 <> col2)%' FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("?column?", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("4"), T("4"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, a CHECK constraint's expression as Postgres prints it; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint - coercion to bool: right to left merges",
            intent: "A check whose expression is an integer passes rows where it isn't zero.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK ((col1 + col2) <> 0));",
                "INSERT INTO t VALUES (1, 1, 1);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col2=0;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1=2;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint - coercion to bool: left to right merges",
            intent: "A check whose expression is an integer passes rows where it isn't zero.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK ((col1 + col2) <> 0));",
                "INSERT INTO t VALUES (1, 1, 1);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col2=0;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col1=2;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - coercion to bool: right to left merges",
            intent: "A check whose expression is an integer records a violation where it is zero.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK ((col1 + col2) <> 0));",
                "INSERT INTO t VALUES (1, 1, 1);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col2=0;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1=0;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, col2, dolt_violation_info::text LIKE '%(col1 + col2)%' AS has_check FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("has_check", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("0"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - coercion to bool: left to right merges",
            intent: "A check whose expression is an integer records a violation where it is zero.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK ((col1 + col2) <> 0));",
                "INSERT INTO t VALUES (1, 1, 1);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col2=0;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col1=0;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, col2, dolt_violation_info::text LIKE '%(col1 + col2)%' AS has_check FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("has_check", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("0"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - schema change: right to left merges",
            intent: "A check violation is found after a schema change on one side.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, col3 int, CHECK (col2 != col3));",
                "INSERT INTO t VALUES (1, 2, 3, -3);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col2=100;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col1;",
                "UPDATE t SET col3=100;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col2, col3, CAST(dolt_violation_info AS text) LIKE '%(col2 <> col3)%' FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col2", INT4), Column("col3", INT4), Column("?column?", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("100"), T("100"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, a CHECK constraint's expression as Postgres prints it; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - schema change: left to right merges",
            intent: "A check violation is found after a schema change on one side.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, col3 int, CHECK (col2 != col3));",
                "INSERT INTO t VALUES (1, 2, 3, -3);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col2=100;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col1;",
                "UPDATE t SET col3=100;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col2, col3, CAST(dolt_violation_info AS text) LIKE '%(col2 <> col3)%' FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col2", INT4), Column("col3", INT4), Column("?column?", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("100"), T("100"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, a CHECK constraint's expression as Postgres prints it; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - deleting rows: right to left merges",
            intent: "Rows deleted on one side are not checked.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, col3 int, CHECK (col2 != col3));",
                "INSERT INTO t VALUES (1, 2, 3, -3);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "DELETE FROM t WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (4, 3, 2, 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - deleting rows: left to right merges",
            intent: "Rows deleted on one side are not checked.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, col3 int, CHECK (col2 != col3));",
                "INSERT INTO t VALUES (1, 2, 3, -3);",
                "CREATE INDEX idx1 ON t (pk, col2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "DELETE FROM t WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (4, 3, 2, 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - divergent edits: right to left merges",
            intent: "Divergent edits that break a new check record violations.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT ('hello'));",
                "INSERT INTO t VALUES (1, 'hi');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD CHECK (col1 != concat('he', 'llo'));",
                "UPDATE t SET col1 = 'bye' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 'adios' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
            name: "check constraint violation - divergent edits: left to right merges",
            intent: "Divergent edits that break a new check record violations.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT ('hello'));",
                "INSERT INTO t VALUES (1, 'hi');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CHECK (col1 != concat('he', 'llo'));",
                "UPDATE t SET col1 = 'bye' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col1 = 'adios' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
            name: "check constraint violation - check is always NULL: right to left merges",
            intent: "A check that is always NULL passes every row.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT ('hello'));",
                "INSERT INTO t VALUES (1, 'hi');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD CHECK (NULL = NULL);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, DEFAULT);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - check is always NULL: left to right merges",
            intent: "A check that is always NULL passes every row.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT ('hello'));",
                "INSERT INTO t VALUES (1, 'hi');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CHECK (NULL = NULL);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (2, DEFAULT);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - check is always false: right to left merges",
            intent: "A check that is always false records a violation for every merged row.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = 1;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT ('hello'));",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD CHECK (1 = 2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, DEFAULT);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
            name: "check constraint violation - check is always false: left to right merges",
            intent: "A check that is always false records a violation for every merged row.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = 1;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT ('hello'));",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CHECK (1 = 2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (1, DEFAULT);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
            name: "check constraint violation - right side violates new check constraint: right to left merges",
            intent: "A check added on one side records violations for the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col00 int, col01 int, col1 varchar(100) DEFAULT ('hello'));",
                "INSERT INTO t VALUES (1, 0, 0, 'hi');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (2, 0, 0, DEFAULT);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col00;",
                "ALTER TABLE t DROP COLUMN col01;",
                "ALTER TABLE t ADD CHECK (col1 != concat('he', 'llo'));",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) LIKE '%((col1)::text <> concat(''he'', ''llo''))%' FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", VARCHAR), Column("?column?", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("2"), T("hello"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, a CHECK constraint's expression as Postgres prints it; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - right side violates new check constraint: left to right merges",
            intent: "A check added on one side records violations for the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col00 int, col01 int, col1 varchar(100) DEFAULT ('hello'));",
                "INSERT INTO t VALUES (1, 0, 0, 'hi');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 0, 0, DEFAULT);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col00;",
                "ALTER TABLE t DROP COLUMN col01;",
                "ALTER TABLE t ADD CHECK (col1 != concat('he', 'llo'));",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) LIKE '%((col1)::text <> concat(''he'', ''llo''))%' FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", VARCHAR), Column("?column?", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("2"), T("hello"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, a CHECK constraint's expression as Postgres prints it; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - keyless table, right side violates new check constraint: right to left merges",
            intent: "A check added on one side records violations for the other side's rows of a keyless table.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (c0 int, col0 varchar(100), col1 varchar(100) DEFAULT ('hello'));",
                "INSERT INTO t VALUES (1, 'adios', 'hi');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (2, 'hola', DEFAULT);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CHECK (col1 != concat('he', 'llo'));",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, c0, col0, col1, CAST(dolt_violation_info AS text) LIKE '%((col1)::text <> concat(''he'', ''llo''))%' FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("c0", INT4), Column("col0", VARCHAR), Column("col1", VARCHAR), Column("?column?", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("2"), T("hola"), T("hello"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, a CHECK constraint's expression as Postgres prints it; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint violation - keyless table, right side violates new check constraint: left to right merges",
            intent: "A check added on one side records violations for the other side's rows of a keyless table.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (c0 int, col0 varchar(100), col1 varchar(100) DEFAULT ('hello'));",
                "INSERT INTO t VALUES (1, 'adios', 'hi');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'hola', DEFAULT);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD CHECK (col1 != concat('he', 'llo'));",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, c0, col0, col1, CAST(dolt_violation_info AS text) LIKE '%((col1)::text <> concat(''he'', ''llo''))%' FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("c0", INT4), Column("col0", VARCHAR), Column("col1", VARCHAR), Column("?column?", BOOL)],
                        rows: &[
                            &[T("check constraint"), T("2"), T("hola"), T("hello"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, a CHECK constraint's expression as Postgres prints it; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a column to a unique index where merged rows violate the new index: right to left merges",
            intent: "Merged rows that violate a widened unique index record violations, which can be resolved and committed.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b smallint); CREATE UNIQUE INDEX uq ON t (a);",
                "INSERT INTO t VALUES (1, 10, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "DROP INDEX uq;",
                "CREATE UNIQUE INDEX uq ON t (a, b);",
                "INSERT INTO t VALUES (2, 20, 2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 20, 2);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, a, b, dolt_violation_info FROM dolt_constraint_violations_t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("a", INT4), Column("b", INT2), Column("dolt_violation_info", JSONB)],
                        rows: &[
                            &[T("unique index"), T("2"), T("20"), T("2"), T("{\"Name\": \"uq\", \"Columns\": [\"a\", \"b\"]}")],
                            &[T("unique index"), T("3"), T("20"), T("2"), T("{\"Name\": \"uq\", \"Columns\": [\"a\", \"b\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("constraint violation")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = 30 WHERE pk = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'merge right, resolve unique index violation');",
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
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", INT2)],
                        rows: &[
                            &[T("1"), T("10"), T("1")],
                            &[T("2"), T("20"), T("2")],
                            &[T("3"), T("30"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a column to a unique index where merged rows violate the new index: left to right merges",
            intent: "Merged rows that violate a widened unique index record violations, which can be resolved and committed.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b smallint); CREATE UNIQUE INDEX uq ON t (a);",
                "INSERT INTO t VALUES (1, 10, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "DROP INDEX uq;",
                "CREATE UNIQUE INDEX uq ON t (a, b);",
                "INSERT INTO t VALUES (2, 20, 2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 20, 2);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, a, b, dolt_violation_info FROM dolt_constraint_violations_t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("a", INT4), Column("b", INT2), Column("dolt_violation_info", JSONB)],
                        rows: &[
                            &[T("unique index"), T("2"), T("20"), T("2"), T("{\"Name\": \"uq\", \"Columns\": [\"a\", \"b\"]}")],
                            &[T("unique index"), T("3"), T("20"), T("2"), T("{\"Name\": \"uq\", \"Columns\": [\"a\", \"b\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("constraint violation")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = 30 WHERE pk = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'merge right, resolve unique index violation');",
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
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", INT2)],
                        rows: &[
                            &[T("1"), T("10"), T("1")],
                            &[T("2"), T("20"), T("2")],
                            &[T("3"), T("30"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a column to a unique index and inserting data valid only under the new index: right to left merges",
            intent: "Rows valid only under a widened unique index merge.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b smallint); CREATE UNIQUE INDEX uq ON t (a);",
                "INSERT INTO t VALUES (1, 10, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "DROP INDEX uq;",
                "CREATE UNIQUE INDEX uq ON t (a, b);",
                "INSERT INTO t VALUES (2, 10, 2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 30, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", INT2)],
                        rows: &[
                            &[T("1"), T("10"), T("1")],
                            &[T("2"), T("10"), T("2")],
                            &[T("3"), T("30"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a column to a unique index and inserting data valid only under the new index: left to right merges",
            intent: "Rows valid only under a widened unique index merge.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, a int, b smallint); CREATE UNIQUE INDEX uq ON t (a);",
                "INSERT INTO t VALUES (1, 10, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "DROP INDEX uq;",
                "CREATE UNIQUE INDEX uq ON t (a, b);",
                "INSERT INTO t VALUES (2, 10, 2);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 30, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", INT2)],
                        rows: &[
                            &[T("1"), T("10"), T("1")],
                            &[T("2"), T("10"), T("2")],
                            &[T("3"), T("30"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_schema_change_tests_schema_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "type widening: right to left merges",
            intent: "Columns widened to larger types on one side keep the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 text, col2 float, col3 smallint, col4 decimal(4,2), col5 varchar(10), col6 SET('a', 'b'), col7 bit(1));",
                "INSERT INTO t VALUES (1, 'blue', 1.0, 1, 0.1, 'one', 'a,b', 1);",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE text, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col2 TYPE double precision, ALTER COLUMN col2 DROP NOT NULL, ALTER COLUMN col2 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col3 TYPE bigint, ALTER COLUMN col3 DROP NOT NULL, ALTER COLUMN col3 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col4 TYPE decimal(8,4), ALTER COLUMN col4 DROP NOT NULL, ALTER COLUMN col4 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col5 TYPE varchar(20), ALTER COLUMN col5 DROP NOT NULL, ALTER COLUMN col5 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col6 TYPE SET('a', 'b', 'c'), ALTER COLUMN col6 DROP NOT NULL, ALTER COLUMN col6 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col7 TYPE bit(2), ALTER COLUMN col7 DROP NOT NULL, ALTER COLUMN col7 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, 'red', 3.0, 420, 0.001, 'three', 'a,b,c', 3);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'green', 2.0, 2, 0.2, 'two', 'a,b', 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, our_schema, their_schema, base_schema FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("our_schema", TEXT), Column("their_schema", TEXT), Column("base_schema", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green'),\n  `col2` float,\n  `col3` smallint,\n  `col4` decimal(4,2),\n  `col5` varchar(10),\n  `col6` set('a','b'),\n  `col7` bit(1),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green','red'),\n  `col2` double,\n  `col3` bigint,\n  `col4` decimal(8,4),\n  `col5` varchar(20),\n  `col6` set('a','b','c'),\n  `col7` bit(2),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green'),\n  `col2` float,\n  `col3` smallint,\n  `col4` decimal(4,2),\n  `col5` varchar(10),\n  `col6` set('a','b'),\n  `col7` bit(1),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: SET types are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "type widening: left to right merges",
            intent: "Columns widened to larger types on one side keep the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 text, col2 float, col3 smallint, col4 decimal(4,2), col5 varchar(10), col6 SET('a', 'b'), col7 bit(1));",
                "INSERT INTO t VALUES (1, 'blue', 1.0, 1, 0.1, 'one', 'a,b', 1);",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE text, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col2 TYPE double precision, ALTER COLUMN col2 DROP NOT NULL, ALTER COLUMN col2 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col3 TYPE bigint, ALTER COLUMN col3 DROP NOT NULL, ALTER COLUMN col3 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col4 TYPE decimal(8,4), ALTER COLUMN col4 DROP NOT NULL, ALTER COLUMN col4 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col5 TYPE varchar(20), ALTER COLUMN col5 DROP NOT NULL, ALTER COLUMN col5 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col6 TYPE SET('a', 'b', 'c'), ALTER COLUMN col6 DROP NOT NULL, ALTER COLUMN col6 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col7 TYPE bit(2), ALTER COLUMN col7 DROP NOT NULL, ALTER COLUMN col7 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, 'red', 3.0, 420, 0.001, 'three', 'a,b,c', 3);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (2, 'green', 2.0, 2, 0.2, 'two', 'a,b', 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, their_schema, our_schema, base_schema FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("their_schema", TEXT), Column("our_schema", TEXT), Column("base_schema", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green'),\n  `col2` float,\n  `col3` smallint,\n  `col4` decimal(4,2),\n  `col5` varchar(10),\n  `col6` set('a','b'),\n  `col7` bit(1),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green','red'),\n  `col2` double,\n  `col3` bigint,\n  `col4` decimal(8,4),\n  `col5` varchar(20),\n  `col6` set('a','b','c'),\n  `col7` bit(2),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green'),\n  `col2` float,\n  `col3` smallint,\n  `col4` decimal(4,2),\n  `col5` varchar(10),\n  `col6` set('a','b'),\n  `col7` bit(1),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: SET types are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "type shortening: right to left merges",
            intent: "Columns narrowed on one side convert the other side's rows that fit.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 text, col2 double precision, col3 bigint, col4 decimal(8,4), col5 varchar(20), col6 SET('a','b','c'), col7 bit(2));",
                "INSERT INTO t VALUES (3, 'green', 3.0, 420, 0.001, 'three', 'a,b', 1);",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE text, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col2 TYPE float, ALTER COLUMN col2 DROP NOT NULL, ALTER COLUMN col2 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col3 TYPE smallint, ALTER COLUMN col3 DROP NOT NULL, ALTER COLUMN col3 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col4 TYPE decimal(4,2), ALTER COLUMN col4 DROP NOT NULL, ALTER COLUMN col4 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col5 TYPE varchar(10), ALTER COLUMN col5 DROP NOT NULL, ALTER COLUMN col5 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col6 TYPE SET('a', 'b'), ALTER COLUMN col6 DROP NOT NULL, ALTER COLUMN col6 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col7 TYPE bit(1), ALTER COLUMN col7 DROP NOT NULL, ALTER COLUMN col7 DROP DEFAULT;",
                "INSERT INTO t VALUES (1, 'blue', 1.0, 1, 0.1, 'one', 'a,b', 1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'green', 2.0, 2, 0.2, 'two', 'a,b', 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, our_schema, their_schema, base_schema FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("our_schema", TEXT), Column("their_schema", TEXT), Column("base_schema", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green','red'),\n  `col2` double,\n  `col3` bigint,\n  `col4` decimal(8,4),\n  `col5` varchar(20),\n  `col6` set('a','b','c'),\n  `col7` bit(2),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green'),\n  `col2` float,\n  `col3` smallint,\n  `col4` decimal(4,2),\n  `col5` varchar(10),\n  `col6` set('a','b'),\n  `col7` bit(1),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green','red'),\n  `col2` double,\n  `col3` bigint,\n  `col4` decimal(8,4),\n  `col5` varchar(20),\n  `col6` set('a','b','c'),\n  `col7` bit(2),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: SET types are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "type shortening: left to right merges",
            intent: "Columns narrowed on one side convert the other side's rows that fit.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 text, col2 double precision, col3 bigint, col4 decimal(8,4), col5 varchar(20), col6 SET('a','b','c'), col7 bit(2));",
                "INSERT INTO t VALUES (3, 'green', 3.0, 420, 0.001, 'three', 'a,b', 1);",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE text, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col2 TYPE float, ALTER COLUMN col2 DROP NOT NULL, ALTER COLUMN col2 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col3 TYPE smallint, ALTER COLUMN col3 DROP NOT NULL, ALTER COLUMN col3 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col4 TYPE decimal(4,2), ALTER COLUMN col4 DROP NOT NULL, ALTER COLUMN col4 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col5 TYPE varchar(10), ALTER COLUMN col5 DROP NOT NULL, ALTER COLUMN col5 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col6 TYPE SET('a', 'b'), ALTER COLUMN col6 DROP NOT NULL, ALTER COLUMN col6 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col7 TYPE bit(1), ALTER COLUMN col7 DROP NOT NULL, ALTER COLUMN col7 DROP DEFAULT;",
                "INSERT INTO t VALUES (1, 'blue', 1.0, 1, 0.1, 'one', 'a,b', 1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (2, 'green', 2.0, 2, 0.2, 'two', 'a,b', 1);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, their_schema, our_schema, base_schema FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("their_schema", TEXT), Column("our_schema", TEXT), Column("base_schema", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green','red'),\n  `col2` double,\n  `col3` bigint,\n  `col4` decimal(8,4),\n  `col5` varchar(20),\n  `col6` set('a','b','c'),\n  `col7` bit(2),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green'),\n  `col2` float,\n  `col3` smallint,\n  `col4` decimal(4,2),\n  `col5` varchar(10),\n  `col6` set('a','b'),\n  `col7` bit(1),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` enum('blue','green','red'),\n  `col2` double,\n  `col3` bigint,\n  `col4` decimal(8,4),\n  `col5` varchar(20),\n  `col6` set('a','b','c'),\n  `col7` bit(2),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: SET types are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "duplicate index tag set: right to left merges",
            intent: "Two indexes on one column added on both sides merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100));",
                "INSERT INTO t VALUES (1, '100'), (2, '200');",
                "CREATE UNIQUE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "CREATE INDEX idx2 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '300');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, our_schema, their_schema, base_schema, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("our_schema", TEXT), Column("their_schema", TEXT), Column("base_schema", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100),\n  PRIMARY KEY (`pk`),\n  UNIQUE KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100),\n  PRIMARY KEY (`pk`),\n  UNIQUE KEY `idx1` (`col1`),\n  KEY `idx2` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100),\n  PRIMARY KEY (`pk`),\n  UNIQUE KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("multiple indexes covering the same column set cannot be merged: 'idx1' and 'idx2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "duplicate index tag set: left to right merges",
            intent: "Two indexes on one column added on both sides merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100));",
                "INSERT INTO t VALUES (1, '100'), (2, '200');",
                "CREATE UNIQUE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE INDEX idx2 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, '300');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, their_schema, our_schema, base_schema, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("their_schema", TEXT), Column("our_schema", TEXT), Column("base_schema", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100),\n  PRIMARY KEY (`pk`),\n  UNIQUE KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100),\n  PRIMARY KEY (`pk`),\n  UNIQUE KEY `idx1` (`col1`),\n  KEY `idx2` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100),\n  PRIMARY KEY (`pk`),\n  UNIQUE KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("multiple indexes covering the same column set cannot be merged: 'idx1' and 'idx2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "index conflicts: both sides add an index with the same name, same columns, but different type: right to left merges",
            intent: "Indexes of one name and columns but different kinds conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "CREATE INDEX idx1 ON t (col2(2));",
                "INSERT INTO t VALUES (1, 10, '100');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE INDEX idx1 ON t (col2);",
                "INSERT INTO t VALUES (2, 20, '200');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, base_schema, our_schema, their_schema FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("base_schema", TEXT), Column("our_schema", TEXT), Column("their_schema", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int,\n  `col2` varchar(100),\n  PRIMARY KEY (`pk`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int,\n  `col2` varchar(100),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col2`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int,\n  `col2` varchar(100),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col2`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "index conflicts: both sides add an index with the same name, same columns, but different type: left to right merges",
            intent: "Indexes of one name and columns but different kinds conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE INDEX idx1 ON t (col2(2));",
                "INSERT INTO t VALUES (1, 10, '100');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "CREATE INDEX idx1 ON t (col2);",
                "INSERT INTO t VALUES (2, 20, '200');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, base_schema, their_schema, our_schema FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("base_schema", TEXT), Column("their_schema", TEXT), Column("our_schema", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int,\n  `col2` varchar(100),\n  PRIMARY KEY (`pk`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int,\n  `col2` varchar(100),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col2`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int,\n  `col2` varchar(100),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col2`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "modifying a column on one side of a merge, and deleting it on the other: right to left merges",
            intent: "A column retyped on one side and dropped on the other is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t(i int PRIMARY KEY, j int);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN j;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN j TYPE varchar(24) USING j::text, ALTER COLUMN j DROP NOT NULL, ALTER COLUMN j DROP DEFAULT;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t")],
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
            name: "modifying a column on one side of a merge, and deleting it on the other: left to right merges",
            intent: "A column retyped on one side and dropped on the other is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t(i int PRIMARY KEY, j int);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN j;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN j TYPE varchar(24) USING j::text, ALTER COLUMN j DROP NOT NULL, ALTER COLUMN j DROP DEFAULT;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t")],
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
            name: "type changes to a column on both sides of a merge: right to left merges",
            intent: "A column retyped differently on both sides is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t(i int PRIMARY KEY, j int);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN j TYPE varchar(100), ALTER COLUMN j DROP NOT NULL, ALTER COLUMN j DROP DEFAULT;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN j TYPE float USING j::text::float, ALTER COLUMN j DROP NOT NULL, ALTER COLUMN j DROP DEFAULT;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t")],
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
            name: "type changes to a column on both sides of a merge: left to right merges",
            intent: "A column retyped differently on both sides is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t(i int PRIMARY KEY, j int);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN j TYPE varchar(100), ALTER COLUMN j DROP NOT NULL, ALTER COLUMN j DROP DEFAULT;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN j TYPE float USING j::text::float, ALTER COLUMN j DROP NOT NULL, ALTER COLUMN j DROP DEFAULT;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t")],
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
            name: "changing the type of a column: right to left merges",
            intent: "A column retyped on one side converts the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(100), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, 'thirty'), (4, 'forty');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("thirty")],
                            &[T("4"), T("forty")],
                            &[T("5"), T("50")],
                            &[T("6"), T("60")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "changing the type of a column: left to right merges",
            intent: "A column retyped on one side converts the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(100), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, 'thirty'), (4, 'forty');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("thirty")],
                            &[T("4"), T("forty")],
                            &[T("5"), T("50")],
                            &[T("6"), T("60")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "changing the type of a column with an index: right to left merges",
            intent: "A retyped indexed column keeps its index over the converted rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int); CREATE INDEX col1_idx ON t (col1);",
                "INSERT INTO t VALUES (1, 100), (2, 20);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(100), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, 'thirty'), (4, 'forty');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("20")],
                            &[T("3"), T("thirty")],
                            &[T("4"), T("forty")],
                            &[T("5"), T("50")],
                            &[T("6"), T("60")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE col1 = '50';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "changing the type of a column with an index: left to right merges",
            intent: "A retyped indexed column keeps its index over the converted rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int); CREATE INDEX col1_idx ON t (col1);",
                "INSERT INTO t VALUES (1, 100), (2, 20);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(100), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, 'thirty'), (4, 'forty');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("20")],
                            &[T("3"), T("thirty")],
                            &[T("4"), T("forty")],
                            &[T("5"), T("50")],
                            &[T("6"), T("60")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE col1 = '50';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping column on right shifts column index between compatible types: right to left merges",
            intent: "A dropped column doesn't shift other columns' values into its place.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b char(20), c char(20)); CREATE INDEX  ON t (c);",
                "INSERT INTO t VALUES (1, 1, '2', '3');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN b;",
                "UPDATE t SET c = '4';",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET a = 2;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping column on right shifts column index between compatible types: left to right merges",
            intent: "A dropped column doesn't shift other columns' values into its place.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b char(20), c char(20)); CREATE INDEX  ON t (c);",
                "INSERT INTO t VALUES (1, 1, '2', '3');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN b;",
                "UPDATE t SET c = '4';",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET a = 2;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "add a non-nullable column, with no default value: right to left merges",
            intent: "A NOT NULL column without a default can't fill the other side's new rows, which fails the merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col3 int NOT NULL;",
                "CREATE INDEX idx1 ON t (col3, col1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge.ErrUnmergeableNewColumn", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge.ErrUnmergeableNewColumn", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't add a NOT NULL column without a default to a table with rows"),
            ..S
        },
        ScriptTest {
            name: "add a non-nullable column, with no default value: left to right merges",
            intent: "A NOT NULL column without a default can't fill the other side's new rows, which fails the merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col3 int NOT NULL;",
                "CREATE INDEX idx1 ON t (col3, col1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge.ErrUnmergeableNewColumn", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge.ErrUnmergeableNewColumn", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't add a NOT NULL column without a default to a table with rows"),
            ..S
        },
        ScriptTest {
            name: "schema change combined with drop row: right to left merges",
            intent: "A row deleted on one side stays deleted across the other side's schema change.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100)); CREATE UNIQUE INDEX unique1 ON t (col2, pk);",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (pk, col1, col2);",
                "CREATE INDEX idx3 ON t (col1, col2);",
                "CREATE INDEX idx4 ON t (pk, col2);",
                "CREATE INDEX idx5 ON t(col2(2));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t DROP COLUMN col1;",
                "INSERT INTO t VALUES (3, '300'), (4, '400');",
                "DELETE FROM t WHERE pk = 1;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("2"), T("200")],
                            &[T("3"), T("300")],
                            &[T("4"), T("400")],
                            &[T("5"), T("500")],
                            &[T("6"), T("600")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "schema change combined with drop row: left to right merges",
            intent: "A row deleted on one side stays deleted across the other side's schema change.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100)); CREATE UNIQUE INDEX unique1 ON t (col2, pk);",
                "INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (pk, col1, col2);",
                "CREATE INDEX idx3 ON t (col1, col2);",
                "CREATE INDEX idx4 ON t (pk, col2);",
                "CREATE INDEX idx5 ON t(col2(2));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN col1;",
                "INSERT INTO t VALUES (3, '300'), (4, '400');",
                "DELETE FROM t WHERE pk = 1;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("2"), T("200")],
                            &[T("3"), T("300")],
                            &[T("4"), T("400")],
                            &[T("5"), T("500")],
                            &[T("6"), T("600")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "adding a non-null column with a default value to one side: right to left merges",
            intent: "A NOT NULL column with a default fills the other side's new rows.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 int NOT NULL DEFAULT 0;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "INSERT INTO t VALUES (2, 2, 2, NULL);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("0"), Null],
                            &[T("2"), T("2"), T("2"), Null],
                            &[T("3"), T("3"), T("0"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_violation_type FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_violation_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a non-null column with a default value to one side: left to right merges",
            intent: "A NOT NULL column with a default fills the other side's new rows.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 int NOT NULL DEFAULT 0;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "INSERT INTO t VALUES (2, 2, 2, NULL);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("0"), Null],
                            &[T("2"), T("2"), T("2"), Null],
                            &[T("3"), T("3"), T("0"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_violation_type FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_violation_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a non-null column with a default value to one side (with update to existing row): right to left merges",
            intent: "A NOT NULL column with a default fills new rows and keeps updated ones.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 int NOT NULL DEFAULT 0;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "UPDATE t SET col2 = 1 WHERE pk = 1;",
                "INSERT INTO t VALUES (2, 2, 2, NULL);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), Null],
                            &[T("2"), T("2"), T("2"), Null],
                            &[T("3"), T("3"), T("0"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt skips it: its merge gives row (1, 1, 0, NULL), dropping their update of col2")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_violation_type FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_violation_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a non-null column with a default value to one side (with update to existing row): left to right merges",
            intent: "A NOT NULL column with a default fills new rows and keeps updated ones.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 int NOT NULL DEFAULT 0;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "UPDATE t SET col2 = 1 WHERE pk = 1;",
                "INSERT INTO t VALUES (2, 2, 2, NULL);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), Null],
                            &[T("2"), T("2"), T("2"), Null],
                            &[T("3"), T("3"), T("0"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt skips it: its merge gives row (1, 1, 0, NULL), dropping their update of col2")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_violation_type FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_violation_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a not-null constraint and default value to a column: right to left merges",
            intent: "A NOT NULL and default added to a column apply to the other side's rows.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, NULL), (2, NULL);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col1 = 9999 WHERE col1 IS NULL;",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 SET DEFAULT 9999;",
                "INSERT INTO t VALUES (3, 30), (4, 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, NULL), (6, NULL);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("9999")],
                            &[T("2"), T("9999")],
                            &[T("3"), T("30")],
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_violation_type FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_violation_type", TEXT)],
                        rows: &[
                            &[T("5"), T("not null")],
                            &[T("6"), T("not null")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a not-null constraint and default value to a column: left to right merges",
            intent: "A NOT NULL and default added to a column apply to the other side's rows.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, NULL), (2, NULL);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 9999 WHERE col1 IS NULL;",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 SET DEFAULT 9999;",
                "INSERT INTO t VALUES (3, 30), (4, 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, NULL), (6, NULL);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("9999")],
                            &[T("2"), T("9999")],
                            &[T("3"), T("30")],
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_violation_type FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_violation_type", TEXT)],
                        rows: &[
                            &[T("5"), T("not null")],
                            &[T("6"), T("not null")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a not-null constraint and default value to a column, alongside table rewrite: right to left merges",
            intent: "A NOT NULL and default added with a table rewrite apply to the other side's rows.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, NULL), (2, NULL);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col1 = 9999 WHERE col1 IS NULL;",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 SET DEFAULT 9999;",
                "ALTER TABLE t ADD COLUMN col2 int DEFAULT 100;",
                "INSERT INTO t VALUES (3, 30, 200), (4, 40, 300);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, NULL), (6, NULL);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("9999"), T("100")],
                            &[T("2"), T("9999"), T("100")],
                            &[T("3"), T("30"), T("200")],
                            &[T("4"), T("40"), T("300")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_violation_type FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_violation_type", TEXT)],
                        rows: &[
                            &[T("5"), T("not null")],
                            &[T("6"), T("not null")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a not-null constraint and default value to a column, alongside table rewrite: left to right merges",
            intent: "A NOT NULL and default added with a table rewrite apply to the other side's rows.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, NULL), (2, NULL);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 9999 WHERE col1 IS NULL;",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 SET DEFAULT 9999;",
                "ALTER TABLE t ADD COLUMN col2 int DEFAULT 100;",
                "INSERT INTO t VALUES (3, 30, 200), (4, 40, 300);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (5, NULL), (6, NULL);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("9999"), T("100")],
                            &[T("2"), T("9999"), T("100")],
                            &[T("3"), T("30"), T("200")],
                            &[T("4"), T("40"), T("300")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_violation_type FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_violation_type", TEXT)],
                        rows: &[
                            &[T("5"), T("not null")],
                            &[T("6"), T("not null")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a not-null constraint to one side: right to left merges",
            intent: "A NOT NULL added on one side records not-null violations for the other side's NULL rows.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, NULL), (2, NULL);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET col1 = 0 WHERE col1 IS NULL;",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, NULL);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("dolt_violation_info", JSONB)],
                        rows: &[
                            &[T("not null"), T("3"), T("{\"Columns\": [\"col1\"]}")],
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
            name: "adding a not-null constraint to one side: left to right merges",
            intent: "A NOT NULL added on one side records not-null violations for the other side's NULL rows.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, NULL), (2, NULL);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 0 WHERE col1 IS NULL;",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, NULL);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("dolt_violation_info", JSONB)],
                        rows: &[
                            &[T("not null"), T("3"), T("{\"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_schema_change_tests_generated_columns() {
    run_scripts(&[
        ScriptTest {
            name: "reordering a column: right to left merges",
            intent: "A column moved to another position on one side merges by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100) GENERATED ALWAYS AS (concat(col1, 'hello')) stored);",
                "INSERT INTO t (pk, col1) VALUES (1, 10), (2, 20);",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (col2);",
                "CREATE INDEX idx3 ON t (pk, col1, col2);",
                "CREATE INDEX idx4 ON t (col1, col2);",
                "CREATE INDEX idx5 ON t (col2, col1);",
                "CREATE INDEX idx6 ON t (col2, pk, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int after col2, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t (pk, col1) VALUES (3, 30), (4, 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (pk, col1) VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("10hello")],
                            &[T("2"), T("20"), T("20hello")],
                            &[T("3"), T("30"), T("30hello")],
                            &[T("4"), T("40"), T("40hello")],
                            &[T("5"), T("50"), T("50hello")],
                            &[T("6"), T("60"), T("60hello")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't move a column to another position"),
            ..S
        },
        ScriptTest {
            name: "reordering a column: left to right merges",
            intent: "A column moved to another position on one side merges by column identity.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100) GENERATED ALWAYS AS (concat(col1, 'hello')) stored);",
                "INSERT INTO t (pk, col1) VALUES (1, 10), (2, 20);",
                "CREATE INDEX idx1 ON t (pk, col1);",
                "CREATE INDEX idx2 ON t (col2);",
                "CREATE INDEX idx3 ON t (pk, col1, col2);",
                "CREATE INDEX idx4 ON t (col1, col2);",
                "CREATE INDEX idx5 ON t (col2, col1);",
                "CREATE INDEX idx6 ON t (col2, pk, col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int after col2, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t (pk, col1) VALUES (3, 30), (4, 40);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t (pk, col1) VALUES (5, 50), (6, 60);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("10"), T("10hello")],
                            &[T("2"), T("20"), T("20hello")],
                            &[T("3"), T("30"), T("30hello")],
                            &[T("4"), T("40"), T("40hello")],
                            &[T("5"), T("50"), T("50hello")],
                            &[T("6"), T("60"), T("60hello")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't move a column to another position"),
            ..S
        },
        ScriptTest {
            name: "adding columns to a table with a virtual column: right to left merges",
            intent: "Columns added to a table with a virtual column keep it computing.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int GENERATED ALWAYS AS (pk + 1));",
                "INSERT INTO t (pk) VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "INSERT INTO t (pk, col2, col3) VALUES (2, 4, 5);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (pk) VALUES (3);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2, col3 FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null],
                            &[T("2"), T("3"), T("4"), T("5")],
                            &[T("3"), T("4"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding columns to a table with a virtual column: left to right merges",
            intent: "Columns added to a table with a virtual column keep it computing.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int GENERATED ALWAYS AS (pk + 1));",
                "INSERT INTO t (pk) VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "INSERT INTO t (pk, col2, col3) VALUES (2, 4, 5);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t (pk) VALUES (3);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2, col3 FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null],
                            &[T("2"), T("3"), T("4"), T("5")],
                            &[T("3"), T("4"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a virtual column to one side, regular columns to other side: right to left merges",
            intent: "A virtual column on one side computes over columns the other side added.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t (pk) VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 1);",
                "INSERT INTO t (pk) VALUES (3);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "INSERT INTO t (pk, col2, col3) VALUES (2, 4, 5);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2, col3 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null],
                            &[T("2"), T("3"), T("4"), T("5")],
                            &[T("3"), T("4"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a virtual column to one side, regular columns to other side: left to right merges",
            intent: "A virtual column on one side computes over columns the other side added.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t (pk) VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 1);",
                "INSERT INTO t (pk) VALUES (3);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "ALTER TABLE t ADD COLUMN col3 int;",
                "INSERT INTO t (pk, col2, col3) VALUES (2, 4, 5);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2, col3 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("2"), Null, Null],
                            &[T("2"), T("3"), T("4"), T("5")],
                            &[T("3"), T("4"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a virtual column to one side: right to left merges",
            intent: "A virtual column added on one side computes for the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t (pk) VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 1);",
                "INSERT INTO t (pk) VALUES (3);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (pk) VALUES (2);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a virtual column to one side: left to right merges",
            intent: "A virtual column added on one side computes for the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t (pk) VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 1);",
                "INSERT INTO t (pk) VALUES (3);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t (pk) VALUES (2);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a stored generated column to one side: right to left merges",
            intent: "A stored generated column added on one side is computed for the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t (pk) VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 1) stored;",
                "INSERT INTO t (pk) VALUES (3);",
                "CREATE INDEX idx1 ON t (col1, pk);",
                "CREATE INDEX idx2 ON t (col1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (pk) VALUES (2);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding a stored generated column to one side: left to right merges",
            intent: "A stored generated column added on one side is computed for the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t (pk) VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 1) stored;",
                "INSERT INTO t (pk) VALUES (3);",
                "CREATE INDEX idx1 ON t (col1, pk);",
                "CREATE INDEX idx2 ON t (col1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t (pk) VALUES (2);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding generated columns to both sides: right to left merges",
            intent: "Generated columns added on each side are both computed for the merged rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100) GENERATED ALWAYS AS (pk::text || 'hello');",
                "INSERT INTO t (pk) VALUES (3), (4);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 100) stored;",
                "INSERT INTO t (pk) VALUES (5), (6);",
                "CREATE INDEX ON t (col1);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("101"), T("1hello")],
                            &[T("2"), T("102"), T("2hello")],
                            &[T("3"), T("103"), T("3hello")],
                            &[T("4"), T("104"), T("4hello")],
                            &[T("5"), T("105"), T("5hello")],
                            &[T("6"), T("106"), T("6hello")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding generated columns to both sides: left to right merges",
            intent: "Generated columns added on each side are both computed for the merged rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100) GENERATED ALWAYS AS (pk::text || 'hello');",
                "INSERT INTO t (pk) VALUES (3), (4);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 100) stored;",
                "INSERT INTO t (pk) VALUES (5), (6);",
                "CREATE INDEX ON t (col1);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("101"), T("1hello")],
                            &[T("2"), T("102"), T("2hello")],
                            &[T("3"), T("103"), T("3hello")],
                            &[T("4"), T("104"), T("4hello")],
                            &[T("5"), T("105"), T("5hello")],
                            &[T("6"), T("106"), T("6hello")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding virtual columns to both sides: right to left merges",
            intent: "Virtual columns added on each side both compute for the merged rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100) GENERATED ALWAYS AS (pk::text || 'hello');",
                "INSERT INTO t (pk) VALUES (3), (4);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 100);",
                "INSERT INTO t (pk) VALUES (5), (6);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("101"), T("1hello")],
                            &[T("2"), T("102"), T("2hello")],
                            &[T("3"), T("103"), T("3hello")],
                            &[T("4"), T("104"), T("4hello")],
                            &[T("5"), T("105"), T("5hello")],
                            &[T("6"), T("106"), T("6hello")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "adding virtual columns to both sides: left to right merges",
            intent: "Virtual columns added on each side both compute for the merged rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN col2 varchar(100) GENERATED ALWAYS AS (pk::text || 'hello');",
                "INSERT INTO t (pk) VALUES (3), (4);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 100);",
                "INSERT INTO t (pk) VALUES (5), (6);",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("101"), T("1hello")],
                            &[T("2"), T("102"), T("2hello")],
                            &[T("3"), T("103"), T("3hello")],
                            &[T("4"), T("104"), T("4hello")],
                            &[T("5"), T("105"), T("5hello")],
                            &[T("6"), T("106"), T("6hello")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "convergent schema changes with virtual columns: right to left merges",
            intent: "The same schema changes with virtual columns on both sides merge into one.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ADD COLUMN col3 int GENERATED ALWAYS AS (pk + 1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ADD COLUMN col3 int GENERATED ALWAYS AS (pk + 1);",
                "UPDATE t SET col1=-1000 WHERE t.pk = 1;",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int NOT NULL,\n  `col3` int GENERATED ALWAYS AS ((pk + 1)),\n  PRIMARY KEY (`pk`)\n  KEY `idx1` (`col3`,`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("-1000"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "convergent schema changes with virtual columns: left to right merges",
            intent: "The same schema changes with virtual columns on both sides merge into one.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ADD COLUMN col3 int GENERATED ALWAYS AS (pk + 1);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE int, ALTER COLUMN col1 SET NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ADD COLUMN col3 int GENERATED ALWAYS AS (pk + 1);",
                "UPDATE t SET col1=-1000 WHERE t.pk = 1;",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` int NOT NULL,\n  `col3` int GENERATED ALWAYS AS ((pk + 1)),\n  PRIMARY KEY (`pk`)\n  KEY `idx1` (`col3`,`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("-1000"), T("2")],
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

#[test]
fn test_schema_change_tests_for_json_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "json merge succeeds without @@dolt_dont_merge_json: right to left merges",
            intent: "Changes to different keys of one JSON document merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{}');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET j = '{\"a\": 1}';",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET j = '{\"b\": 2}';",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("j", JSON)],
                        rows: &[
                            &[T("1"), T("{\"a\": 1, \"b\": 2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json merge succeeds without @@dolt_dont_merge_json: left to right merges",
            intent: "Changes to different keys of one JSON document merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{}');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET j = '{\"a\": 1}';",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET j = '{\"b\": 2}';",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("j", JSON)],
                        rows: &[
                            &[T("1"), T("{\"a\": 1, \"b\": 2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json merge fails with @@dolt_dont_merge_json: right to left merges",
            intent: "With dolt_dont_merge_json on, changes to one JSON document on both sides conflict.",
            set_up_script: &[
                "BEGIN;",
                "SET dolt_dont_merge_json = 1;",
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{}');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET j = '{\"a\": 1}';",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET j = '{\"b\": 2}';",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_j, our_j, their_j FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_j", JSON), Column("our_j", JSON), Column("their_j", JSON)],
                        rows: &[
                            &[T("{}"), T("{\"b\": 2}"), T("{\"a\": 1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "json merge fails with @@dolt_dont_merge_json: left to right merges",
            intent: "With dolt_dont_merge_json on, changes to one JSON document on both sides conflict.",
            set_up_script: &[
                "BEGIN;",
                "SET dolt_dont_merge_json = 1;",
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{}');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET j = '{\"a\": 1}';",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE t SET j = '{\"b\": 2}';",
                "SELECT dolt_commit('-Am', 'left commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_j, their_j, our_j FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_j", JSON), Column("their_j", JSON), Column("our_j", JSON)],
                        rows: &[
                            &[T("{}"), T("{\"b\": 2}"), T("{\"a\": 1}")],
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

#[test]
fn test_schema_change_tests_type_changes() {
    run_scripts(&[
        ScriptTest {
            name: "varchar widening",
            intent: "A varchar widened on one side keeps the other side's rows and takes longer values.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(10));",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(100), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345678901234567890');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("123")],
                            &[T("2"), T("12345678901234567890")],
                            &[T("3"), T("321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "show CREATE TABLE t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("t"), T("CREATE TABLE `t` (\n  `pk` int NOT NULL,\n  `col1` varchar(100),\n  PRIMARY KEY (`pk`),\n  KEY `idx1` (`col1`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJJKLMNOPQRSTUVWXYZ!@#$%^&*()_+');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "enums and sets widening",
            intent: "Enum and set types widened on one side keep the other side's rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 text, col2 SET('blue', 'green'));",
                "INSERT INTO t VALUES (1, 'blue', 'blue,green');",
                "CREATE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE text, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "ALTER TABLE t ALTER COLUMN col2 TYPE SET('blue', 'green', 'red'), ALTER COLUMN col2 DROP NOT NULL, ALTER COLUMN col2 DROP DEFAULT;",
                "INSERT INTO t VALUES (3, 'red', 'red,blue');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 'green', 'green,blue');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("1"), T("blue"), T("blue,green")],
                            &[T("2"), T("green"), T("blue,green")],
                            &[T("3"), T("red"), T("blue,red")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: SET types are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "TEXT index without schema change",
            intent: "An index on a text column merges both sides' rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int NOT NULL PRIMARY KEY, col1 VARCHAR(255) NOT NULL, col2 text);",
                "CREATE INDEX idx ON t (col2(255));",
                "INSERT INTO t VALUES (1, 'aa01010aa', 'aa01010aa');",
                "INSERT INTO t VALUES (2, 'bb01010bb', 'bb01010bb');",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (3, 'cc01010cc', 'cc01010cc');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (4, 'dd01010dd', 'dd01010dd');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t WHERE (col2 LIKE '%01010%') ORDER BY col2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR), Column("col2", TEXT)],
                        rows: &[
                            &[T("1"), T("aa01010aa"), T("aa01010aa")],
                            &[T("2"), T("bb01010bb"), T("bb01010bb")],
                            &[T("3"), T("cc01010cc"), T("cc01010cc")],
                            &[T("4"), T("dd01010dd"), T("dd01010dd")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR), Column("col2", TEXT)],
                        rows: &[
                            &[T("1"), T("aa01010aa"), T("aa01010aa")],
                            &[T("2"), T("bb01010bb"), T("bb01010bb")],
                            &[T("3"), T("cc01010cc"), T("cc01010cc")],
                            &[T("4"), T("dd01010dd"), T("dd01010dd")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT col1 FROM t ORDER by col1;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", VARCHAR)],
                        rows: &[
                            &[T("aa01010aa")],
                            &[T("bb01010bb")],
                            &[T("cc01010cc")],
                            &[T("dd01010dd")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "VARCHAR to TEXT widening with right side modification",
            intent: "A varchar widened to text keeps the other side's changes.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(10), col2 int);",
                "INSERT INTO t VALUES (1, '54321', 10);",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE TEXT, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "UPDATE t SET col2 = 40 WHERE col2 = 10;",
                "INSERT INTO t VALUES (2, '43210000000', 20);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321', 30);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", TEXT), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("54321"), T("40")],
                            &[T("2"), T("43210000000"), T("20")],
                            &[T("3"), T("321"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT col1 FROM t ORDER by col1;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", TEXT)],
                        rows: &[
                            &[T("321")],
                            &[T("43210000000")],
                            &[T("54321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "VARCHAR to TEXT widening",
            intent: "A varchar widened to text keeps the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(10));",
                "INSERT INTO t VALUES (1, '54321');",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE TEXT, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '43210000000');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", TEXT)],
                        rows: &[
                            &[T("1"), T("54321")],
                            &[T("2"), T("43210000000")],
                            &[T("3"), T("321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT col1 FROM t ORDER by col1;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", TEXT)],
                        rows: &[
                            &[T("321")],
                            &[T("43210000000")],
                            &[T("54321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "VARCHAR index with prefix narrowing",
            intent: "A varchar narrowed under a prefix index merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(10));",
                "INSERT INTO t VALUES (1, '987654321');",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "DROP INDEX idx1;",
                "CREATE INDEX idx1 ON t (col1(5));",
                "INSERT INTO t VALUES (2, '87654321');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '7654321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("987654321")],
                            &[T("2"), T("87654321")],
                            &[T("3"), T("7654321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT col1 FROM t ORDER by col1;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", VARCHAR)],
                        rows: &[
                            &[T("7654321")],
                            &[T("87654321")],
                            &[T("987654321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "TEXT index with prefix narrowing",
            intent: "A text column narrowed under a prefix index merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 text);",
                "INSERT INTO t VALUES (1, '987654321');",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "DROP INDEX idx1;",
                "CREATE INDEX idx1 ON t (col1(5));",
                "INSERT INTO t VALUES (2, '87654321');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '7654321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", TEXT)],
                        rows: &[
                            &[T("1"), T("987654321")],
                            &[T("2"), T("87654321")],
                            &[T("3"), T("7654321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT col1 FROM t ORDER by col1;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", TEXT)],
                        rows: &[
                            &[T("7654321")],
                            &[T("87654321")],
                            &[T("987654321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "VARBINARY to BLOB widening",
            intent: "A varbinary widened to a blob keeps the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 bytea);",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE bytea, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345678901234567890');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", BYTEA)],
                        rows: &[
                            &[T("1"), Any],
                            &[T("2"), Any],
                            &[T("3"), Any],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: the binary and tiny text types are MySQL's; Postgres has text and bytea"),
            ..S
        },
        ScriptTest {
            name: "varchar(300) to TINYTEXT(255) narrowing",
            intent: "A varchar narrowed to tinytext converts the rows that fit.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(300));",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE text, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345678901234567890');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'right', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts WHERE description LIKE 'incompatible column types for column ''col1''%';",
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
            skip: Some("MySQL-only: the binary and tiny text types are MySQL's; Postgres has text and bytea"),
            ..S
        },
        ScriptTest {
            name: "schema conflict: VARBINARY(300) to TINYBLOB(255)",
            intent: "A varbinary narrowed to tinyblob is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 bytea);",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE bytea, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345678901234567890');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts WHERE description LIKE 'incompatible column types for column ''col1''%';",
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
            skip: Some("MySQL-only: the binary and tiny text types are MySQL's; Postgres has text and bytea"),
            ..S
        },
        ScriptTest {
            name: "CHAR(5) to TINYTEXT widening",
            intent: "A char widened to tinytext keeps the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 char(5));",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE INDEX idx1 ON t (col1(5));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE text, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345678901234567890');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", TEXT)],
                        rows: &[
                            &[T("1"), T("123")],
                            &[T("2"), T("12345678901234567890")],
                            &[T("3"), T("321")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: the binary and tiny text types are MySQL's; Postgres has text and bytea"),
            ..S
        },
        ScriptTest {
            name: "CHAR(5) to TINYTEXT, different charsets",
            intent: "A char widened to tinytext of another charset is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 char(5) );",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE INDEX idx1 ON t (col1(3));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE text , ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345678901234567890');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT count(*) FROM dolt_schema_conflicts WHERE description LIKE 'incompatible column types for column ''col1''%';",
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
            skip: Some("MySQL-only: the binary and tiny text types are MySQL's; Postgres has text and bytea"),
            ..S
        },
        ScriptTest {
            name: "varchar narrowing",
            intent: "A varchar narrowed on one side converts the other side's rows that fit.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(10));",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(9), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, description LIKE 'incompatible column types for column ''col1''%' FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("?column?", BOOL)],
                        rows: &[
                            &[T("public.t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "TEXT to VARCHAR narrowing",
            intent: "A text column narrowed to varchar converts the other side's rows that fit.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 TEXT);",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(9), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT table_name, description LIKE 'incompatible column types for column ''col1''%' FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("?column?", BOOL)],
                        rows: &[
                            &[T("public.t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: prefix indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "VARCHAR to CHAR widening",
            intent: "A varchar changed to char keeps the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 VARCHAR(10));",
                "INSERT INTO t VALUES (1, '123');",
                "CREATE UNIQUE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE CHAR(11), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '12345');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '321');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT * FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", BPCHAR)],
                        rows: &[
                            &[T("1"), T("123        ")],
                            &[T("2"), T("12345      ")],
                            &[T("3"), T("321        ")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("char(n) values keep their blank padding, as Postgres shows them, where MySQL trims it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BINARY to VARBINARY widening",
            intent: "A binary widened to varbinary keeps the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 bytea);",
                "INSERT INTO t VALUES (1, '\\x01'::bytea);",
                "CREATE UNIQUE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE bytea, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '\\x0102'::bytea);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '\\x010203'::bytea);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", BYTEA)],
                        rows: &[
                            &[T("1"), Any],
                            &[T("2"), Any],
                            &[T("3"), Any],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "VARBINARY to BINARY widening",
            intent: "A varbinary changed to binary pads the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 bytea);",
                "INSERT INTO t VALUES (1, '\\x01'::bytea);",
                "CREATE UNIQUE INDEX idx1 ON t (col1);",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE bytea, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '\\x0102'::bytea);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '\\x010203'::bytea);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", BYTEA)],
                        rows: &[
                            &[T("1"), Any],
                            &[T("2"), Any],
                            &[T("3"), Any],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "TINYBLOB to BINARY(300) widening",
            intent: "A tinyblob widened to binary keeps the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 bytea);",
                "INSERT INTO t VALUES (1, '\\x01'::bytea);",
                "CREATE UNIQUE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE bytea, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, '\\x0102'::bytea);",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, '\\x010203'::bytea);",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, length(col1) FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("1"), T("255")],
                            &[T("2"), T("255")],
                            &[T("3"), T("255")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: the binary and tiny text types are MySQL's; Postgres has text and bytea"),
            ..S
        },
        ScriptTest {
            name: "TINYTEXT to VARCHAR(300) widening",
            intent: "A tinytext widened to varchar keeps the other side's rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 text);",
                "INSERT INTO t VALUES (1, 'tiny tiny text');",
                "CREATE UNIQUE INDEX idx1 ON t (col1(10));",
                "SELECT dolt_commit('-Am', 'ancestor commit');",
                "SELECT dolt_branch('right');",
                "SELECT * FROM dolt_checkout('right');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE VARCHAR(300), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, 'more teeny tiny text');",
                "SELECT dolt_commit('-Am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 'the teeniest of tiny text');",
                "SELECT dolt_commit('-Am', 'left commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT pk, col1 FROM t ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tiny tiny text")],
                            &[T("2"), T("more teeny tiny text")],
                            &[T("3"), T("the teeniest of tiny text")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: the binary and tiny text types are MySQL's; Postgres has text and bytea"),
            ..S
        },
    ]);
}
