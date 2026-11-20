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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_merge_bases.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_merge_bases_table_function_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_merge_bases",
            intent: "dolt_merge_bases returns every best common ancestor of two revisions, in Dolt's order, and checks its arguments.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'main');",
                "SELECT dolt_branch('branchA');",
                "SELECT dolt_branch('branchB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'baseA');",
                "SELECT dolt_tag('baseA');",
                "SELECT * FROM dolt_checkout('branchB');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'baseB');",
                "SELECT dolt_tag('baseB');",
                "SELECT * FROM dolt_merge('baseA', '-m', 'mergeB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT * FROM dolt_merge('baseB', '-m', 'mergeA');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT set_config('test.left', ('branchA')::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT tg.tag_name FROM (SELECT merge_base, ROW_NUMBER() OVER () AS rn FROM dolt_merge_bases('branchA', 'branchB')) b JOIN dolt_tags tg ON tg.tag_hash = b.merge_base ORDER BY b.rn;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("baseB")],
                            &[T("baseA")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tg.tag_name FROM (SELECT merge_base, ROW_NUMBER() OVER () AS rn FROM dolt_merge_bases('branchB', 'branchA')) b JOIN dolt_tags tg ON tg.tag_hash = b.merge_base ORDER BY b.rn;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("baseB")],
                            &[T("baseA")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT merge_base = dolt_hashof('main') FROM dolt_merge_bases('main', 'branchA');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_merge_bases(CONCAT('branch', 'A'), CONCAT('branch', UPPER('b')));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_merge_bases(current_setting('test.left'), 'branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_merge_bases(dolt_hashof('branchA'), dolt_hashof('branchB'));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_merge_bases(NULL, 'branchB');",
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
                    query: "SELECT count(*) FROM dolt_merge_bases('branchA', NULL);",
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
                    query: "SELECT * FROM dolt_merge_bases(1, 'branchB');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_merge_bases(integer, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("dolt_merge_bases resolves by argument types as Postgres functions do, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge_bases('branchA');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_merge_bases' expected 2 arguments, 1 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge_bases('branchA', 'branchB', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "function 'dolt_merge_bases' expected 2 arguments, 3 received", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT merge_base = dolt_hashof('baseA') FROM dolt_merge_bases('branchA~1', 'branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT merge_base = dolt_hashof('baseB') FROM dolt_merge_bases('baseB', 'branchA');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge_bases('branchA', 'nonexistent');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: nonexistent", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tg.tag_name FROM dolt_merge_bases('branchA', 'branchB') b JOIN dolt_tags tg ON tg.tag_hash = b.merge_base ORDER BY tg.tag_name;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("baseA")],
                            &[T("baseB")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('branchA') WHERE commit_hash IN (SELECT merge_base FROM dolt_merge_bases('branchA', 'branchB'));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT br.name, count(*) FROM dolt_branches br JOIN LATERAL (SELECT merge_base FROM dolt_merge_bases(br.name, 'branchB')) b ON true GROUP BY br.name ORDER BY br.name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("branchA"), T("2")],
                            &[T("branchB"), T("1")],
                            &[T("main"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("a join without ON is MySQL's, so the lateral join says ON true")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branchA');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branchA'")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE of the database's branch is a checkout of the branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_merge_bases('HEAD', 'branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT merge_base = dolt_hashof('main') FROM dolt_merge_bases('HEAD~2', 'branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
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
    ]);
}

#[test]
fn test_three_dot_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "three-dot ranges with several merge bases",
            intent: "A three-dot range of the diff functions uses one of several merge bases and warns which, while a range with one merge base, a two-dot range, and dolt_log warn nothing.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'main');",
                "SELECT dolt_branch('branchA');",
                "SELECT dolt_branch('branchB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'baseA');",
                "SELECT dolt_tag('baseA');",
                "SELECT * FROM dolt_checkout('branchB');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'baseB');",
                "SELECT dolt_tag('baseB');",
                "SELECT * FROM dolt_merge('baseA', '-m', 'mergeB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT * FROM dolt_merge('baseB', '-m', 'mergeA');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, to_id FROM dolt_diff('branchA...branchB', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("to_id", INT4)],
                        rows: &[
                            &[T("added"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "branchA...branchB: multiple merge bases, using ", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, to_id FROM dolt_diff('branchB...branchA', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("to_id", INT4)],
                        rows: &[
                            &[T("added"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "branchB...branchA: multiple merge bases, using ", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, rows_added FROM dolt_diff_stat('branchA...branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_added", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "branchA...branchB: multiple merge bases, using ", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_table_name, data_change FROM dolt_diff_summary('branchA...branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT), Column("data_change", BOOL)],
                        rows: &[
                            &[T("public.t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "branchA...branchB: multiple merge bases, using ", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('branchA...branchB');",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("INSERT INTO public.t (id) VALUES (1);")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "branchA...branchB: multiple merge bases, using ", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, to_id FROM dolt_diff('main...branchA', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("to_id", INT4)],
                        rows: &[
                            &[T("added"), T("1")],
                            &[T("added"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW WARNINGS is MySQL's; Postgres sends a warning with the query that raised it, and this one raises none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff('branchA..branchB', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW WARNINGS is MySQL's; Postgres sends a warning with the query that raised it, and this one raises none")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('branchA...branchB') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("mergeA")],
                            &[T("mergeB")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW WARNINGS is MySQL's; Postgres sends a warning with the query that raised it, and this one raises none")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
