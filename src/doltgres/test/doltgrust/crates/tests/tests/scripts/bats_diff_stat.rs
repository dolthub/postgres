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

//! Dolt's integration-tests/bats/diff-stat.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Dolt, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_diff_stat() {
    run_scripts(&[
        ScriptTest {
            name: "diff-stat: stat/summary comparing working table to last commit",
            intent: "dolt_diff_stat and dolt_diff_summary count added, modified, and deleted rows of the working set against HEAD.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "INSERT INTO test VALUES (2, 11, 0, 0, 0, 0);",
                "INSERT INTO test VALUES (3, 11, 0, 0, 0, 0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("2"), T("2"), T("0"), T("0"), T("12"), T("0"), T("0"), T("2"), T("4"), T("12"), T("24")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added two rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 11, 0, 0, 0, 6) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("3"), T("0"), T("0"), T("1"), T("0"), T("0"), T("2"), T("4"), T("4"), T("24"), T("24")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'modified first row');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 0;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("3"), T("0"), T("1"), T("0"), T("0"), T("6"), T("0"), T("4"), T("3"), T("24"), T("18")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
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
            name: "diff-stat: stat/summary comparing row with a deleted cell and an added cell",
            intent: "Setting a cell to NULL and back each count as one modified cell.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.test"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'create table');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'put row');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1, c3, c4, c5) VALUES (0, 1, 3, 4, 5) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("0"), T("0"), T("0"), T("1"), T("0"), T("0"), T("1"), T("1"), T("1"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'row modified');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("0"), T("0"), T("0"), T("1"), T("0"), T("0"), T("1"), T("1"), T("1"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
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
            name: "diff-stat: stat/summary comparing two branches",
            intent: "dolt_diff_stat and dolt_diff_summary compare two branches, also as a two-dot range (the table is committed first, since dolt_checkout leaves the working set on its branch where the CLI carries it).",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_commit('-Am', 'create table test');",
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added one row');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added another row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("1"), T("0"), T("0"), T("6"), T("0"), T("0"), T("1"), T("2"), T("6"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('firstbranch..newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("1"), T("0"), T("0"), T("6"), T("0"), T("0"), T("1"), T("2"), T("6"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('firstbranch..newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
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
            name: "diff-stat: stat/summary shows correct changes after schema change",
            intent: "Cells of a new column count as added for every row, and a modified row counts its changed cells.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE employees (id int PRIMARY KEY, \"first name\" text, \"last name\" text, title text, \"start date\" text, \"end date\" text);",
                "INSERT INTO employees VALUES (0, 'tim', 'sehn', 'ceo', '', ''), (1, 'aaron', 'son', 'founder', '', ''), (2, 'brian', 'hendricks', 'founder', '', '');",
                "SELECT dolt_add('employees');",
                "SELECT dolt_commit('-m', 'Added employees table with data');",
                "ALTER TABLE employees ADD city text;",
                "INSERT INTO employees VALUES (3, 'taylor', 'bantle', 'software engineer', '', '', 'Santa Monica');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.employees"), T("3"), T("1"), T("0"), T("0"), T("10"), T("0"), T("0"), T("3"), T("4"), T("18"), T("28")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.employees"), T("public.employees"), T("modified"), T("t"), T("t")],
                            &[T(""), T("public.test"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO employees VALUES (0, 'tim', 'sehn', 'ceo', '2 years ago', '', 'Santa Monica') ON CONFLICT (id) DO UPDATE SET \"first name\" = EXCLUDED.\"first name\", \"last name\" = EXCLUDED.\"last name\", title = EXCLUDED.title, \"start date\" = EXCLUDED.\"start date\", \"end date\" = EXCLUDED.\"end date\", city = EXCLUDED.city;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.employees"), T("2"), T("1"), T("0"), T("1"), T("10"), T("0"), T("2"), T("3"), T("4"), T("18"), T("28")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.employees"), T("public.employees"), T("modified"), T("t"), T("t")],
                            &[T(""), T("public.test"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff-stat: stat/summary gets summaries for all tables with changes",
            intent: "Without a table, dolt_diff_stat and dolt_diff_summary report every changed table; with one, only it.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "CREATE TABLE employees (id varchar(20) NOT NULL, \"first name\" text, \"last name\" text, title text, \"start date\" text, \"end date\" text, PRIMARY KEY (id));",
                "INSERT INTO employees VALUES (0, 'tim', 'sehn', 'ceo', '', '');",
                "SELECT dolt_add('test', 'employees');",
                "SELECT dolt_commit('-m', 'test tables created');",
                "INSERT INTO test VALUES (2, 11, 0, 0, 0, 0);",
                "INSERT INTO employees VALUES (1, 'brian', 'hendriks', 'founder', '', '');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.employees")],
                            &[T("public.test")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
                            &[T("public.employees"), T("public.employees"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff_stat('HEAD', 'WORKING', 'employees');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.employees")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 'employees');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.employees"), T("public.employees"), T("modified"), T("t"), T("f")],
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
            name: "diff-stat: two and three dot diff stat/summary",
            intent: "A two-dot range compares the tips, and a three-dot range compares from the merge base.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'table');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'row');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (2, 2, 2, 2, 2, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'newrow');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main..branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("1"), T("1"), T("0"), T("6"), T("6"), T("0"), T("2"), T("2"), T("12"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main..branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('main...branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("1"), T("0"), T("0"), T("6"), T("0"), T("0"), T("1"), T("2"), T("6"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('main...branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("t"), T("f")],
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
            name: "diff-stat: diff stat incorrect primary key set change regression test",
            intent: "Dropping a column before the key counts each row as modified, not as a primary key change.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE testdrop (col1 varchar(20), id int PRIMARY KEY, col2 varchar(20));",
                "SELECT dolt_add('.');",
                "INSERT INTO testdrop VALUES ('test1', 1, 'test2');",
                "SELECT dolt_commit('-am', 'Add testdrop table');",
                "ALTER TABLE testdrop DROP COLUMN col1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT rows_modified FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("rows_modified", INT8)],
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
            name: "diff-stat: stat/summary for renamed table",
            intent: "A renamed table has no stat until its rows change, and its summary reports the rename.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "ALTER TABLE test RENAME TO test2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test2"), T("renamed"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test2 VALUES (2, 2, 2, 2, 2, 2);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.test2"), T("2"), T("1"), T("0"), T("0"), T("6"), T("0"), T("0"), T("2"), T("3"), T("12"), T("18")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test2"), T("renamed"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
