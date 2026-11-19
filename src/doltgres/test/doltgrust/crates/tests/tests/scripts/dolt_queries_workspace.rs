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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_workspace.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_workspace_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_workspace_* empty table",
            intent: "dolt_workspace_<t> of a table without changes is empty.",
            set_up_script: &[
                "SET dolt_show_system_tables = on;",
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'dolt_workspace_tbl' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("dolt_id")],
                            &[T("dolt_staged")],
                            &[T("dolt_diff_type")],
                            &[T("to_pk")],
                            &[T("to_val")],
                            &[T("from_pk")],
                            &[T("from_val")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's; information_schema.columns lists the table's columns, Dolt's own ones with the dolt_ prefix, while dolt_show_system_tables is on (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* multiple edits of a single row",
            intent: "Several edits of one row show as one workspace change against HEAD.",
            set_up_script: &[
                "SET dolt_show_system_tables = on;",
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_commit('-am', 'inserting 2 rows at HEAD');",
                "UPDATE tbl SET val=51 WHERE pk=42;",
                "SELECT dolt_add('tbl');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("42"), T("51"), T("42"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'dolt_workspace_tbl' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("dolt_id")],
                            &[T("dolt_staged")],
                            &[T("dolt_diff_type")],
                            &[T("to_pk")],
                            &[T("to_val")],
                            &[T("from_pk")],
                            &[T("from_val")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's; information_schema.columns lists the table's columns, Dolt's own ones with the dolt_ prefix, while dolt_show_system_tables is on (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_TBL;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("42"), T("51"), T("42"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tbl SET val= 108 WHERE pk = 42;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("42"), T("51"), T("42"), T("42")],
                            &[T("1"), T("f"), T("modified"), T("42"), T("108"), T("42"), T("51")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('tbl');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("42"), T("108"), T("42"), T("42")],
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
            name: "dolt_workspace_* single unstaged row",
            intent: "An unstaged modified row shows with staged false and its old and new values.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_commit('-am', 'inserting 2 rows at HEAD');",
                "UPDATE tbl SET val=51 WHERE pk=42;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("42"), T("51"), T("42"), T("42")],
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
            name: "dolt_workspace_* inserted row",
            intent: "An inserted row shows as added with NULL from_ columns.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_commit('-am', 'inserting 2 rows at HEAD');",
                "INSERT INTO tbl VALUES (44,44);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("added"), T("44"), T("44"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('tbl');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("44"), T("44"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tbl SET val = 108 WHERE pk = 44;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("44"), T("44"), Null, Null],
                            &[T("1"), T("f"), T("modified"), T("44"), T("108"), T("44"), T("44")],
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
            name: "dolt_workspace_* deleted row",
            intent: "A deleted row shows as removed with NULL to_ columns.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_commit('-am', 'inserting 2 rows at HEAD');",
                "DELETE FROM tbl WHERE pk = 42;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("removed"), Null, Null, T("42"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('tbl');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, Null, T("42"), T("42")],
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
            name: "dolt_workspace_* clean workspace",
            intent: "A clean working set shows no workspace rows.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_commit('-am', 'inserting 2 rows at HEAD');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_unknowntable;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"dolt_workspace_unknowntable\" does not exist", position: 15, ..E }),
                    dolt: Some(Dolt::Bug("Dolt returns an empty table for a table that doesn't exist, where its own TODO says it should fail as dolt_diff_<t> does (dolthub/dolt#11139)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* created table",
            intent: "A created table's rows show as added.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("added"), T("42"), T("42"), Null, Null],
                            &[T("1"), T("f"), T("added"), T("43"), T("43"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('tbl');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("42"), T("42"), Null, Null],
                            &[T("1"), T("t"), T("added"), T("43"), T("43"), Null, Null],
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
            name: "dolt_workspace_* dropped table",
            intent: "A dropped table's rows show as removed.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_commit('-am', 'inserting rows 3 rows at HEAD');",
                "DROP TABLE tbl;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("removed"), Null, Null, T("42"), T("42")],
                            &[T("1"), T("f"), T("removed"), Null, Null, T("43"), T("43")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    skip: Some("2f core: dolt_workspace_<t> of a table the working set dropped, which has no per-table view while the catalog synthesizes them for its own tables"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('tbl');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, Null, T("42"), T("42")],
                            &[T("1"), T("t"), T("removed"), Null, Null, T("43"), T("43")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    skip: Some("2f core: dolt_workspace_<t> of a table the working set dropped, which has no per-table view while the catalog synthesizes them for its own tables"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* keyless table",
            intent: "A table without a primary key shows its changed rows.",
            set_up_script: &[
                "CREATE TABLE tbl (x int, y int);",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (42,42);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_x", INT4), Column("to_y", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("added"), T("42"), T("42"), Null, Null],
                            &[T("1"), T("f"), T("added"), T("42"), T("42"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('tbl');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_x", INT4), Column("to_y", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("42"), T("42"), Null, Null],
                            &[T("1"), T("t"), T("added"), T("42"), T("42"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tbl VALUES (42,42);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_x", INT4), Column("to_y", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("42"), T("42"), Null, Null],
                            &[T("1"), T("t"), T("added"), T("42"), T("42"), Null, Null],
                            &[T("2"), T("f"), T("added"), T("42"), T("42"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* schema change",
            intent: "A schema change shows rows under the new columns.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_commit('-am', 'inserting rows 3 rows at HEAD');",
                "UPDATE tbl SET val=51 WHERE pk=42;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("42"), T("51"), T("42"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tbl ADD COLUMN newcol CHAR(36);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("to_newcol", BPCHAR), Column("from_pk", INT4), Column("from_val", INT4), Column("from_newcol", BPCHAR)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("42"), T("51"), Null, T("42"), T("42"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('tbl');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("to_newcol", BPCHAR), Column("from_pk", INT4), Column("from_val", INT4), Column("from_newcol", BPCHAR)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("42"), T("51"), Null, T("42"), T("42"), Null],
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
            name: "dolt_workspace_* prevent illegal updates",
            intent: "Only the staged column of dolt_workspace_<t> can be updated.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO tbl VALUES (42,42);",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "UPDATE tbl SET val=51 WHERE pk=42;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET to_val = 108 WHERE dolt_id = 0;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only update of column 'dolt_staged' is allowed", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* modifies promote to staging",
            intent: "Setting staged on a modified row stages it.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO tbl VALUES (41,42,43);",
                "INSERT INTO tbl VALUES (50,51,52);",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "UPDATE tbl SET x=23;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("41"), T("23"), T("43"), T("41"), T("42"), T("43")],
                            &[T("1"), T("f"), T("modified"), T("50"), T("23"), T("52"), T("50"), T("51"), T("52")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE to_pk = 41;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("41"), T("23"), T("43"), T("41"), T("42"), T("43")],
                            &[T("1"), T("f"), T("modified"), T("50"), T("23"), T("52"), T("50"), T("51"), T("52")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE dolt_staged = FALSE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("41"), T("23"), T("43"), T("41"), T("42"), T("43")],
                            &[T("1"), T("t"), T("modified"), T("50"), T("23"), T("52"), T("50"), T("51"), T("52")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tbl SET y=81;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("41"), T("23"), T("43"), T("41"), T("42"), T("43")],
                            &[T("1"), T("t"), T("modified"), T("50"), T("23"), T("52"), T("50"), T("51"), T("52")],
                            &[T("2"), T("f"), T("modified"), T("41"), T("23"), T("81"), T("41"), T("23"), T("43")],
                            &[T("3"), T("f"), T("modified"), T("50"), T("23"), T("81"), T("50"), T("23"), T("52")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(y) FROM tbl AS OF STAGED;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("95")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(y) FROM tbl AS OF WORKING;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("162")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("41"), T("23"), T("81"), T("41"), T("42"), T("43")],
                            &[T("1"), T("t"), T("modified"), T("50"), T("23"), T("81"), T("50"), T("51"), T("52")],
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
            name: "dolt_workspace_* inserts promote to staging",
            intent: "Setting staged on an added row stages it.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, x int, y int);",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "INSERT INTO tbl VALUES (41,42,43);",
                "INSERT INTO tbl VALUES (50,51,52);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("added"), T("41"), T("42"), T("43"), Null, Null, Null],
                            &[T("1"), T("f"), T("added"), T("50"), T("51"), T("52"), Null, Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE to_pk = 41;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("41"), T("42"), T("43"), Null, Null, Null],
                            &[T("1"), T("f"), T("added"), T("50"), T("51"), T("52"), Null, Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE dolt_staged = FALSE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("41"), T("42"), T("43"), Null, Null, Null],
                            &[T("1"), T("t"), T("added"), T("50"), T("51"), T("52"), Null, Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tbl SET x=81;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("41"), T("42"), T("43"), Null, Null, Null],
                            &[T("1"), T("t"), T("added"), T("50"), T("51"), T("52"), Null, Null, Null],
                            &[T("2"), T("f"), T("modified"), T("41"), T("81"), T("43"), T("41"), T("42"), T("43")],
                            &[T("3"), T("f"), T("modified"), T("50"), T("81"), T("52"), T("50"), T("51"), T("52")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_x", INT4), Column("to_y", INT4), Column("from_pk", INT4), Column("from_x", INT4), Column("from_y", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("41"), T("81"), T("43"), Null, Null, Null],
                            &[T("1"), T("t"), T("added"), T("50"), T("81"), T("52"), Null, Null, Null],
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
            name: "dolt_workspace_* deletes promote to staging",
            intent: "Setting staged on a removed row stages the delete.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY);",
                "INSERT INTO tbl VALUES (41);",
                "INSERT INTO tbl VALUES (50);",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "DELETE FROM tbl;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("from_pk", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("removed"), Null, T("41")],
                            &[T("1"), T("f"), T("removed"), Null, T("50")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE dolt_id = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("from_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, T("41")],
                            &[T("1"), T("f"), T("removed"), Null, T("50")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE dolt_staged = FALSE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("from_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, T("41")],
                            &[T("1"), T("t"), T("removed"), Null, T("50")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tbl VALUES (41);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("from_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, T("41")],
                            &[T("1"), T("t"), T("removed"), Null, T("50")],
                            &[T("2"), T("f"), T("added"), T("41"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("from_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, T("50")],
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
            name: "dolt_workspace_* modifies downgrade from staging",
            intent: "Clearing staged on a staged modification unstages it.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, v char(36));",
                "INSERT INTO tbl VALUES (42, gen_random_uuid());",
                "INSERT INTO tbl VALUES (23, gen_random_uuid());",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "UPDATE tbl SET v = gen_random_uuid();",
                "SELECT dolt_add('tbl');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("23")],
                            &[T("1"), T("t"), T("modified"), T("42")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE WHERE dolt_id = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("42")],
                            &[T("1"), T("f"), T("modified"), T("23")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE WHERE dolt_staged = TRUE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("23")],
                            &[T("1"), T("f"), T("modified"), T("42")],
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
            name: "dolt_workspace_* inserts downgrade from staging",
            intent: "Clearing staged on a staged insert unstages it.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, v char(36));",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "INSERT INTO tbl VALUES (42, gen_random_uuid());",
                "INSERT INTO tbl VALUES (23, gen_random_uuid());",
                "SELECT dolt_add('tbl');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("23")],
                            &[T("1"), T("t"), T("added"), T("42")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE WHERE dolt_id = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("42")],
                            &[T("1"), T("f"), T("added"), T("23")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE WHERE dolt_staged = TRUE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("added"), T("23")],
                            &[T("1"), T("f"), T("added"), T("42")],
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
            name: "dolt_workspace_* deletes downgrade from staging",
            intent: "Clearing staged on a staged delete unstages it.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, v char(36));",
                "INSERT INTO tbl VALUES (42, gen_random_uuid());",
                "INSERT INTO tbl VALUES (23, gen_random_uuid());",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "DELETE FROM tbl;",
                "SELECT dolt_add('tbl');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, from_pk, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), T("23"), Null],
                            &[T("1"), T("t"), T("removed"), T("42"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE WHERE from_pk = 23;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, from_pk, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), T("42"), Null],
                            &[T("1"), T("f"), T("removed"), T("23"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, from_pk, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("removed"), T("23"), Null],
                            &[T("1"), T("f"), T("removed"), T("42"), Null],
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
            name: "dolt_workspace_* complicated mixed updates",
            intent: "Staging and unstaging a mix of inserts, updates, and deletes leaves each row in the asked state.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, v char(36));",
                "INSERT INTO tbl VALUES (42, gen_random_uuid());",
                "INSERT INTO tbl VALUES (23, gen_random_uuid());",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "UPDATE tbl SET v = gen_random_uuid() WHERE pk = 42;",
                "DELETE FROM tbl WHERE pk = 23;",
                "SELECT dolt_add('tbl');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, from_pk, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), T("23"), Null],
                            &[T("1"), T("t"), T("modified"), T("42"), T("42")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tbl SET v = gen_random_uuid() WHERE pk = 42;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, from_pk, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), T("23"), Null],
                            &[T("1"), T("t"), T("modified"), T("42"), T("42")],
                            &[T("2"), T("f"), T("modified"), T("42"), T("42")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tbl VALUES (23, gen_random_uuid());",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, from_pk, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), T("23"), Null],
                            &[T("1"), T("t"), T("modified"), T("42"), T("42")],
                            &[T("2"), T("f"), T("added"), Null, T("23")],
                            &[T("3"), T("f"), T("modified"), T("42"), T("42")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE dolt_id = 2;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, from_pk, to_pk FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("23"), T("23")],
                            &[T("1"), T("t"), T("modified"), T("42"), T("42")],
                            &[T("2"), T("f"), T("modified"), T("42"), T("42")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* downgrades keep working changes",
            intent: "Unstaging a row keeps its working changes.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, v varchar(20));",
                "INSERT INTO tbl VALUES (42, 'inserted');",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "UPDATE tbl SET v = 'staged';",
                "SELECT dolt_add('tbl');",
                "UPDATE tbl SET v = 'working';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_v", VARCHAR), Column("from_pk", INT4), Column("from_v", VARCHAR)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("42"), T("staged"), T("42"), T("inserted")],
                            &[T("1"), T("f"), T("modified"), T("42"), T("working"), T("42"), T("staged")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE WHERE dolt_id = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_v", VARCHAR), Column("from_pk", INT4), Column("from_v", VARCHAR)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("42"), T("working"), T("42"), T("inserted")],
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
            name: "dolt_workspace_* keyless tables",
            intent: "Rows of a table without a primary key stage and unstage individually.",
            set_up_script: &[
                "CREATE TABLE tbl (val int);",
                "INSERT INTO tbl VALUES (42);",
                "INSERT INTO tbl VALUES (42);",
                "INSERT INTO tbl VALUES (42);",
                "INSERT INTO tbl VALUES (51);",
                "INSERT INTO tbl VALUES (51);",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "UPDATE tbl SET val=51 WHERE ctid = (SELECT ctid FROM tbl WHERE val=42 LIMIT 1);",
                "SELECT dolt_add('tbl');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_val", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("51"), Null],
                            &[T("1"), T("t"), T("removed"), Null, T("42")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val, count(*) AS num FROM tbl AS OF STAGED  GROUP BY val ORDER BY val;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4), Column("num", INT8)],
                        rows: &[
                            &[T("42"), T("2")],
                            &[T("51"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE WHERE dolt_id = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_val", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, T("42")],
                            &[T("1"), T("f"), T("added"), T("51"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tbl SET val=23;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val, count(*) AS num FROM tbl AS OF STAGED  GROUP BY val ORDER BY val;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4), Column("num", INT8)],
                        rows: &[
                            &[T("42"), T("2")],
                            &[T("51"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val, count(*) AS num FROM tbl AS OF WORKING GROUP BY val ORDER BY val;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4), Column("num", INT8)],
                        rows: &[
                            &[T("23"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_val", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, T("42")],
                            &[T("1"), T("f"), T("removed"), Null, T("51")],
                            &[T("2"), T("f"), T("removed"), Null, T("51")],
                            &[T("3"), T("f"), T("removed"), Null, T("42")],
                            &[T("4"), T("f"), T("removed"), Null, T("42")],
                            &[T("5"), T("f"), T("added"), T("23"), Null],
                            &[T("6"), T("f"), T("added"), T("23"), Null],
                            &[T("7"), T("f"), T("added"), T("23"), Null],
                            &[T("8"), T("f"), T("added"), T("23"), Null],
                            &[T("9"), T("f"), T("added"), T("23"), Null],
                        ],
                        tag: "SELECT 10",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26, and a table without a primary key lists its rows in their hash order, which differs from Dolt's, so they take other dolt_id numbers, Appendix C \"order of rows from a scan of a table without ORDER BY\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE dolt_id % 2 = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_val", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("removed"), Null, T("51")],
                            &[T("1"), T("t"), T("removed"), Null, T("42")],
                            &[T("2"), T("t"), T("removed"), Null, T("42")],
                            &[T("3"), T("t"), T("added"), T("23"), Null],
                            &[T("4"), T("t"), T("added"), T("23"), Null],
                            &[T("5"), T("f"), T("removed"), Null, T("51")],
                            &[T("6"), T("f"), T("removed"), Null, T("42")],
                            &[T("7"), T("f"), T("added"), T("23"), Null],
                            &[T("8"), T("f"), T("added"), T("23"), Null],
                            &[T("9"), T("f"), T("added"), T("23"), Null],
                        ],
                        tag: "SELECT 10",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26, and a table without a primary key lists its rows in their hash order, which differs from Dolt's, so they take other dolt_id numbers, Appendix C \"order of rows from a scan of a table without ORDER BY\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val, count(*) AS num FROM tbl AS OF STAGED  GROUP BY val ORDER BY val;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4), Column("num", INT8)],
                        rows: &[
                            &[T("23"), T("2")],
                            &[T("42"), T("1")],
                            &[T("51"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val, count(*) AS num FROM tbl AS OF WORKING GROUP BY val ORDER BY val;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4), Column("num", INT8)],
                        rows: &[
                            &[T("23"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* delete forbidden on staged rows",
            intent: "Deleting a staged row from dolt_workspace_<t> is refused.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_add('tbl');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("42"), T("42"), Null, Null],
                            &[T("1"), T("t"), T("added"), T("43"), T("43"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_tbl WHERE dolt_id = 0;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot delete staged rows from workspace", ..E }),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = FALSE WHERE to_pk = 42;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_tbl WHERE to_pk = 42;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("43"), T("43"), Null, Null],
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
            name: "dolt_workspace_* delete from workspace to restore deleted row",
            intent: "Deleting a removed row's workspace entry restores the row.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_add('tbl');",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "DELETE FROM tbl;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_tbl WHERE dolt_id = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("removed"), Null, Null, T("43"), T("43")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("42"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* delete from workspace to remove new row",
            intent: "Deleting an added row's workspace entry removes the row.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_tbl WHERE to_pk = 42;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("added"), T("43"), T("43"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("43"), T("43")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* delete from workspace to revert and update",
            intent: "Deleting a modified row's workspace entry reverts the row.",
            set_up_script: &[
                "CREATE TABLE tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (43,43);",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "UPDATE tbl SET val=val*2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("42"), T("84"), T("42"), T("42")],
                            &[T("1"), T("f"), T("modified"), T("43"), T("86"), T("43"), T("43")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_tbl WHERE to_pk = 42;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("43"), T("86"), T("43"), T("43")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("42"), T("42")],
                            &[T("43"), T("86")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* schema change partial staging unstaging",
            intent: "Reverting every row through a table's workspace while its columns differ from HEAD's converts the staged rows to its definition by column id and keeps the schema change, after which no row is left to stage.",
            set_up_script: &[
                "CREATE TABLE tbl (x int PRIMARY KEY, y int);",
                "INSERT INTO tbl VALUES (42,42);",
                "INSERT INTO tbl VALUES (23,23);",
                "SELECT dolt_commit('-Am', 'creating table tbl');",
                "ALTER TABLE tbl ADD COLUMN why varchar(16);",
                "UPDATE tbl SET why = 'fourty two' WHERE x = 42;",
                "UPDATE tbl SET why = 'twenty three' WHERE x = 23;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_x", INT4), Column("to_y", INT4), Column("to_why", VARCHAR), Column("from_x", INT4), Column("from_y", INT4), Column("from_why", VARCHAR)],
                        rows: &[
                            &[T("0"), T("f"), T("modified"), T("23"), T("23"), T("twenty three"), T("23"), T("23"), Null],
                            &[T("1"), T("f"), T("modified"), T("42"), T("42"), T("fourty two"), T("42"), T("42"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_tbl;",
                    expected: Expected::Tag("DELETE 2"),
                    dolt: Some(Dolt::Differs("Doltgrust stages a row of a table whose columns changed since HEAD by staging the table's working definition with its staged rows converted by column id, where Dolt refuses the write (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE;",
                    expected: Expected::Tag("UPDATE 0"),
                    dolt: Some(Dolt::Differs("Doltgrust stages a row of a table whose columns changed since HEAD by staging the table's working definition with its staged rows converted by column id, where Dolt refuses the write (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_tbl WHERE dolt_id = 0;",
                    expected: Expected::Tag("DELETE 0"),
                    dolt: Some(Dolt::Differs("Doltgrust stages a row of a table whose columns changed since HEAD by staging the table's working definition with its staged rows converted by column id, where Dolt refuses the write (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_tbl SET dolt_staged = TRUE WHERE dolt_id = 0;",
                    expected: Expected::Tag("UPDATE 0"),
                    dolt: Some(Dolt::Differs("Doltgrust stages a row of a table whose columns changed since HEAD by staging the table's working definition with its staged rows converted by column id, where Dolt refuses the write (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* stage individual rows from new table",
            intent: "Rows of a new table stage one at a time.",
            set_up_script: &[
                "CREATE TABLE new_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO new_tbl VALUES (1, 100);",
                "INSERT INTO new_tbl VALUES (2, 200);",
                "INSERT INTO new_tbl VALUES (3, 300);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_new_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("f"), T("added"), T("1"), T("100"), Null, Null],
                            &[T("1"), T("f"), T("added"), T("2"), T("200"), Null, Null],
                            &[T("2"), T("f"), T("added"), T("3"), T("300"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_new_tbl SET dolt_staged = TRUE WHERE dolt_id = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_new_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("1"), T("100"), Null, Null],
                            &[T("1"), T("f"), T("added"), T("2"), T("200"), Null, Null],
                            &[T("2"), T("f"), T("added"), T("3"), T("300"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_new_tbl SET dolt_staged = TRUE WHERE dolt_id = 2;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_new_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_val", INT4), Column("from_pk", INT4), Column("from_val", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("1"), T("100"), Null, Null],
                            &[T("1"), T("t"), T("added"), T("3"), T("300"), Null, Null],
                            &[T("2"), T("f"), T("added"), T("2"), T("200"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* stage all rows from new table at once",
            intent: "Every row of a new table stages at once.",
            set_up_script: &[
                "CREATE TABLE bulk_tbl (pk int PRIMARY KEY, name varchar(32));",
                "INSERT INTO bulk_tbl VALUES (1, 'alice'), (2, 'bob');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_bulk_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_name", VARCHAR), Column("from_pk", INT4), Column("from_name", VARCHAR)],
                        rows: &[
                            &[T("0"), T("f"), T("added"), T("1"), T("alice"), Null, Null],
                            &[T("1"), T("f"), T("added"), T("2"), T("bob"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_bulk_tbl SET dolt_staged = TRUE;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_workspace_bulk_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_pk", INT4), Column("to_name", VARCHAR), Column("from_pk", INT4), Column("from_name", VARCHAR)],
                        rows: &[
                            &[T("0"), T("t"), T("added"), T("1"), T("alice"), Null, Null],
                            &[T("1"), T("t"), T("added"), T("2"), T("bob"), Null, Null],
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
            name: "dolt_workspace_* delete restricted by foreign key",
            intent: "Reverting a row a foreign key restricts through its workspace entry is refused.",
            set_up_script: &[
                "CREATE TABLE lists (id int PRIMARY KEY, name varchar(100));",
                "CREATE TABLE list_entries (id int PRIMARY KEY, list_id int NOT NULL, content varchar(255), FOREIGN KEY (list_id) REFERENCES lists(id));",
                "SELECT dolt_commit('-Am', 'schema');",
                "INSERT INTO lists VALUES (1, 'list_1');",
                "INSERT INTO list_entries VALUES (1, 1, 'entry_1'), (2, 1, 'entry_2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_lists WHERE to_id = 1;",
                    expected: Expected::Error(Diagnostic {
                        code: "23503",
                        message: "violates foreign key constraint",
                        message_contains: true,
                        detail: "Key (id)=(1) is still referenced from table \"list_entries\".",
                        schema: "public",
                        table: "list_entries",
                        constraint: "list_entries_list_id_fkey",
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM lists ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, list_id FROM list_entries ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("list_id", INT4)],
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
            name: "dolt_workspace_* delete cascades through foreign key",
            intent: "Reverting a row through its workspace entry cascades through a foreign key.",
            set_up_script: &[
                "CREATE TABLE lists (id int PRIMARY KEY, name varchar(100));",
                "CREATE TABLE list_entries (id int PRIMARY KEY, list_id int NOT NULL, content varchar(255), FOREIGN KEY (list_id) REFERENCES lists(id) ON DELETE CASCADE);",
                "SELECT dolt_commit('-Am', 'schema');",
                "INSERT INTO lists VALUES (1, 'list_1');",
                "INSERT INTO list_entries VALUES (1, 1, 'entry_1'), (2, 1, 'entry_2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_lists WHERE to_id = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM lists ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM list_entries ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
            name: "dolt_workspace_* stage a delete and insert that reuse a unique key",
            intent: "Staging a delete and an insert that reuse a unique key succeeds when both are staged together.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, u int); CREATE UNIQUE INDEX ix ON t (u);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO t VALUES (5, 10);",
                "SELECT dolt_commit('-am', 'insert row with u=10');",
                "DELETE FROM t WHERE id = 5;",
                "INSERT INTO t VALUES (1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = TRUE WHERE dolt_diff_type = 'added';",
                    expected: Expected::Error(Diagnostic {
                        code: "23505",
                        message: "duplicate key value violates unique constraint",
                        message_contains: true,
                        detail: "Key (u)=(10) already exists.",
                        schema: "public",
                        table: "t",
                        constraint: "ix",
                        ..E
                    }),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, dolt_staged FROM dolt_workspace_t ORDER BY dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("dolt_staged", BOOL)],
                        rows: &[
                            &[T("added"), T("f")],
                            &[T("removed"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, u FROM t AS OF 'STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("5"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = TRUE;",
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, u FROM t AS OF 'STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t AS OF 'STAGED' WHERE u = 10;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
            name: "dolt_workspace_* stage a delete and insert that reuse a composite unique key",
            intent: "Staging a delete and an insert that reuse a composite unique key succeeds together.",
            set_up_script: &[
                "CREATE TABLE foo (id varchar(36) PRIMARY KEY);",
                "CREATE TABLE bar (id varchar(36) PRIMARY KEY, foo_id varchar(36) NOT NULL, sequence int, FOREIGN KEY (foo_id) REFERENCES foo(id) ON DELETE CASCADE ON UPDATE CASCADE); CREATE UNIQUE INDEX idx_foo_id_sequence ON bar (foo_id, sequence);",
                "SELECT dolt_commit('-Am', 'creating tables foo and bar');",
                "INSERT INTO foo VALUES ('f1');",
                "INSERT INTO bar VALUES ('b1', 'f1', 10);",
                "SELECT dolt_commit('-am', 'insert foo and bar with sequence 10');",
                "DELETE FROM bar WHERE id = 'b1';",
                "INSERT INTO bar VALUES ('b2', 'f1', 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_bar SET dolt_staged = TRUE;",
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sequence FROM bar AS OF 'STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR), Column("sequence", INT4)],
                        rows: &[
                            &[T("b2"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* stage two modifies that swap a unique key",
            intent: "Staging two modifications that swap a unique key succeeds together.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, u int); CREATE UNIQUE INDEX ix ON t (u);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO t VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-am', 'insert two rows');",
                "UPDATE t SET u = 99 WHERE id = 1;",
                "UPDATE t SET u = 10 WHERE id = 2;",
                "UPDATE t SET u = 20 WHERE id = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = TRUE;",
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, u FROM t AS OF 'STAGED' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("20")],
                            &[T("2"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* keyless table stage a delete and insert that reuse a unique key",
            intent: "In a table without a primary key, staging a delete and an insert that reuse a unique key succeeds together.",
            set_up_script: &[
                "CREATE TABLE t (a int, u int); CREATE UNIQUE INDEX ix ON t (u);",
                "SELECT dolt_commit('-Am', 'creating keyless table t');",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-am', 'insert row with u=10');",
                "DELETE FROM t WHERE a = 1;",
                "INSERT INTO t VALUES (2, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = TRUE WHERE dolt_diff_type = 'added';",
                    expected: Expected::Error(Diagnostic {
                        code: "23505",
                        message: "duplicate key value violates unique constraint",
                        message_contains: true,
                        detail: "Key (u)=(10) already exists.",
                        schema: "public",
                        table: "t",
                        constraint: "ix",
                        ..E
                    }),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, dolt_staged FROM dolt_workspace_t ORDER BY dolt_diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("dolt_staged", BOOL)],
                        rows: &[
                            &[T("added"), T("f")],
                            &[T("removed"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, u FROM t AS OF 'STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = TRUE;",
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, u FROM t AS OF 'STAGED';",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("2"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* stage a modify that leaves the unique key untouched",
            intent: "Staging a modification that keeps its unique key succeeds.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, u int, v int); CREATE UNIQUE INDEX ix ON t (u);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO t VALUES (1, 10, 100), (2, 20, 200);",
                "SELECT dolt_commit('-am', 'insert two rows');",
                "UPDATE t SET v = 101 WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "INSERT INTO t VALUES (3, 20, 300);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = TRUE;",
                    expected: Expected::Tag("UPDATE 3"),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, u, v FROM t AS OF 'STAGED' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("u", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("101")],
                            &[T("3"), T("20"), T("300")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_workspace_* stage rows that share a null unique key",
            intent: "Rows sharing a NULL unique key stage together.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, u int); CREATE UNIQUE INDEX ix ON t (u);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-am', 'insert row with u=10');",
                "UPDATE t SET u = NULL WHERE id = 1;",
                "INSERT INTO t VALUES (2, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = TRUE;",
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, u FROM t AS OF 'STAGED' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
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
