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

//! Dolt's integration-tests/bats/keyless.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_keyless() {
    run_scripts(&[
        ScriptTest {
            name: "keyless: merge fast-forward",
            intent: "A fast-forward merge brings a keyless row over.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO keyless VALUES (9, 9);",
                "SELECT dolt_commit('-am', '9,9');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless WHERE c0 > 6 ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge branches with identical mutation history",
            intent: "The same keyless rows added on both sides conflict, and the table keeps one copy of each.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('other');",
                "INSERT INTO keyless VALUES (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO keyless VALUES (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
                    query: "SELECT * FROM keyless WHERE c0 > 6 ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge deletes from two branches",
            intent: "Different keyless rows deleted on each side are both gone after the merge.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('left');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "DELETE FROM keyless WHERE c0 = 0;",
                "SELECT dolt_commit('-am', 'deleted ones on right');",
                "SELECT * FROM dolt_checkout('left');",
                "DELETE FROM keyless WHERE c0 = 2;",
                "SELECT dolt_commit('-am', 'deleted twos on left');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge');",
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
                    query: "SELECT from_c0, from_c1, dolt_diff_type FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY from_c0;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c0", INT4), Column("from_c1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("removed")],
                            &[T("2"), T("2"), T("removed")],
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
            name: "keyless: merge duplicate deletes",
            intent: "Different numbers of copies deleted on each side conflict, and --ours keeps our count.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "CREATE TABLE dupe (c0 int, c1 int);",
                "INSERT INTO dupe (c0, c1) VALUES (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'created table dupe');",
                "SELECT dolt_branch('left');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "DELETE FROM dupe WHERE ctid IN (SELECT ctid FROM dupe LIMIT 2);",
                "SELECT dolt_commit('-am', 'deleted two rows on right');",
                "SELECT * FROM dolt_checkout('left');",
                "DELETE FROM dupe WHERE ctid IN (SELECT ctid FROM dupe LIMIT 4);",
                "SELECT dolt_commit('-am', 'deleted four rows on left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 'dupe');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT sum(c0), sum(c1) FROM dupe;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge duplicate deletes with stored procedure",
            intent: "Different numbers of copies deleted on each side conflict, and dolt_conflicts_resolve with --ours keeps our count.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "CREATE TABLE dupe (c0 int, c1 int);",
                "INSERT INTO dupe (c0, c1) VALUES (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'created table dupe');",
                "SELECT dolt_branch('left');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "DELETE FROM dupe WHERE ctid IN (SELECT ctid FROM dupe LIMIT 2);",
                "SELECT dolt_commit('-am', 'deleted two rows on right');",
                "SELECT * FROM dolt_checkout('left');",
                "DELETE FROM dupe WHERE ctid IN (SELECT ctid FROM dupe LIMIT 4);",
                "SELECT dolt_commit('-am', 'deleted four rows on left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 'dupe');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT sum(c0), sum(c1) FROM dupe;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge duplicate updates",
            intent: "Different numbers of copies updated on each side conflict, and --theirs takes their updates.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "CREATE TABLE dupe (c0 int, c1 int);",
                "INSERT INTO dupe (c0, c1) VALUES (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'created table dupe');",
                "SELECT dolt_branch('left');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE dupe SET c1 = 2 WHERE ctid IN (SELECT ctid FROM dupe LIMIT 2);",
                "SELECT dolt_commit('-am', 'updated two rows on right');",
                "SELECT * FROM dolt_checkout('left');",
                "UPDATE dupe SET c1 = 2 WHERE ctid IN (SELECT ctid FROM dupe LIMIT 4);",
                "SELECT dolt_commit('-am', 'updated four rows on left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'dupe');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT sum(c0), sum(c1) FROM dupe;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("10"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge duplicate updates with stored procedure",
            intent: "Different numbers of copies updated on each side conflict, and dolt_conflicts_resolve with --theirs takes their updates.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "CREATE TABLE dupe (c0 int, c1 int);",
                "INSERT INTO dupe (c0, c1) VALUES (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'created table dupe');",
                "SELECT dolt_branch('left');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE dupe SET c1 = 2 WHERE ctid IN (SELECT ctid FROM dupe LIMIT 2);",
                "SELECT dolt_commit('-am', 'updated two rows on right');",
                "SELECT * FROM dolt_checkout('left');",
                "UPDATE dupe SET c1 = 2 WHERE ctid IN (SELECT ctid FROM dupe LIMIT 4);",
                "SELECT dolt_commit('-am', 'updated four rows on left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'dupe');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT sum(c0), sum(c1) FROM dupe;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("10"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge with in-place updates (branches)",
            intent: "The same keyless rows updated differently on each side conflict, and --ours keeps our updated rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "INSERT INTO keyless VALUES (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'added rows');",
                "SELECT dolt_branch('other');",
                "UPDATE keyless SET c1 = c1 + 10 WHERE c0 > 6;",
                "SELECT dolt_commit('-am', 'updated on main');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE keyless SET c1 = c1 + 20 WHERE c0 > 6;",
                "SELECT dolt_commit('-am', 'updated on other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 'keyless');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT * FROM keyless WHERE c0 > 6 ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("7"), T("27")],
                            &[T("8"), T("28")],
                            &[T("9"), T("29")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Bug("the bats test expects main's rows after --ours on other, which keeps other's")),
                    skip: Some("2f merge: rows without a primary key are keyed by their values, so each side's update is a delete and an add, and the merge keeps both sides' added rows; pairing a keyless row's updates across a merge needs keyless row identity"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge branches with reordered mutation history",
            intent: "The same keyless rows added in another order on each side conflict, and the table keeps one copy of each.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('other');",
                "INSERT INTO keyless VALUES (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO keyless VALUES (9, 9), (8, 8), (7, 7);",
                "SELECT dolt_commit('-am', 'inserted on other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge');",
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
                    query: "SELECT count(*) FROM keyless WHERE c0 > 6;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless WHERE c0 > 6 ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge branches with convergent mutation history",
            intent: "Keyless rows that reach the same values by different edits conflict, and --theirs settles them to those values.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('other');",
                "INSERT INTO keyless VALUES (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO keyless VALUES (9, 19), (8, 8), (7, 17);",
                "UPDATE keyless SET c0 = 7, c1 = 7 WHERE c1 = 19;",
                "UPDATE keyless SET c0 = 9, c1 = 9 WHERE c1 = 17;",
                "SELECT dolt_commit('-am', 'inserted on other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'keyless');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT * FROM keyless WHERE c0 > 6 ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge branches with convergent mutation history with stored procedure",
            intent: "Keyless rows that reach the same values by different edits conflict, and dolt_conflicts_resolve with --theirs settles them.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('other');",
                "INSERT INTO keyless VALUES (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO keyless VALUES (9, 19), (8, 8), (7, 17);",
                "UPDATE keyless SET c0 = 7, c1 = 7 WHERE c1 = 19;",
                "UPDATE keyless SET c0 = 9, c1 = 9 WHERE c1 = 17;",
                "SELECT dolt_commit('-am', 'inserted on other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'keyless');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT * FROM keyless WHERE c0 > 6 ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge branches with offset mutation history",
            intent: "An extra copy added on one side conflicts, and --ours keeps our copies.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('other');",
                "INSERT INTO keyless VALUES (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO keyless VALUES (7, 7), (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 'keyless');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT * FROM keyless WHERE c0 > 6 ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("7"), T("7")],
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge branches with offset mutation history with stored procedure",
            intent: "An extra copy added on one side conflicts, and dolt_conflicts_resolve with --ours keeps our copies.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('other');",
                "INSERT INTO keyless VALUES (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO keyless VALUES (7, 7), (7, 7), (8, 8), (9, 9);",
                "SELECT dolt_commit('-am', 'inserted on other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 'keyless');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT * FROM keyless WHERE c0 > 6 ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("7"), T("7")],
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge delete+add on two branches",
            intent: "A keyless row deleted on one side and added again on the other conflicts, and --theirs deletes it.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('left');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "DELETE FROM keyless WHERE c0 = 2;",
                "SELECT dolt_commit('-am', 'deleted twos on right');",
                "SELECT * FROM dolt_checkout('left');",
                "INSERT INTO keyless VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'inserted twos on left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'keyless');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT * FROM keyless ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: merge delete+add on two branches with stored procedure",
            intent: "A keyless row deleted on one side and added again on the other conflicts, and dolt_conflicts_resolve with --theirs deletes it.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0, 0), (2, 2), (1, 1), (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT dolt_branch('left');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "DELETE FROM keyless WHERE c0 = 2;",
                "SELECT dolt_commit('-am', 'deleted twos on right');",
                "SELECT * FROM dolt_checkout('left');",
                "INSERT INTO keyless VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'inserted twos on left');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge');",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'keyless');",
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
                    query: "SELECT length(dolt_commit('-am', 'resolved')) > 0 AS committed;",
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
                    query: "SELECT * FROM keyless ORDER BY c0, c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: feature indexes and foreign keys",
            intent: "A keyless table takes secondary indexes, and a foreign key referencing one of its columns.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX ON keyless (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fine (a int, b int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON fine (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE worse (a int, b int, FOREIGN KEY (b) REFERENCES keyless(c1));",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "there is no unique constraint matching given keys for referenced table \"keyless\"", ..E }),
                    dolt: Some(Dolt::Differs("MySQL lets a foreign key reference a column with any index, where Postgres needs a unique constraint on it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: create keyless table",
            intent: "A table without a primary key is created with its columns and rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("keyless")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the tables, which pg_tables does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'keyless'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT)],
                        rows: &[
                            &[T("c0"), T("integer")],
                            &[T("c1"), T("integer")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_attribute lists the table's columns and types")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(c0),sum(c1) FROM keyless;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: delete from keyless",
            intent: "DELETE removes the matching keyless row and leaves duplicates of others.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless ORDER BY c0;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: update keyless",
            intent: "UPDATE changes the matching keyless row.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c0 = 9 WHERE c0 = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless ORDER BY c0;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                            &[T("9"), T("2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: column add/drop",
            intent: "Adding and dropping columns of a keyless table keeps its rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE keyless ADD COLUMN c2 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE keyless DROP COLUMN c0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), Null],
                            &[T("1"), Null],
                            &[T("1"), Null],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: table import",
            intent: "Rows loaded into a new keyless table keep their duplicates and NULLs, and updates hash NULLs correctly.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE imported (c0 int, c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so INSERT loads the CSV's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO imported VALUES (0,0),(2,2),(1,1),(1,1),(NULL,9);",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so INSERT loads the CSV's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM imported;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0,c1 FROM imported ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[Null, T("9")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE imported SET c1 = c1 + 10;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0,c1 FROM imported ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("10")],
                            &[T("1"), T("11")],
                            &[T("1"), T("11")],
                            &[T("2"), T("12")],
                            &[Null, T("19")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: table update",
            intent: "Loading rows a keyless table already holds adds duplicates of them.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                    expected: Expected::Tag("INSERT 0 4"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so INSERT loads the CSV's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM keyless;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless ORDER BY c0;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff against working set",
            intent: "The working set's diff of a keyless table lists deleted and added rows, an update as both.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (8,8);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = 9 WHERE c0 = 1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('HEAD', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("added"), Null, Null, T("1"), T("9")],
                            &[T("added"), Null, Null, T("1"), T("9")],
                            &[T("added"), Null, Null, T("8"), T("8")],
                            &[T("removed"), T("0"), T("0"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the diff, which dolt_diff() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff --stat",
            intent: "The diff statistics of a keyless table count its added and deleted rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (8,8);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = 9 WHERE c0 = 1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rows_added, rows_deleted FROM dolt_diff_stat('HEAD', 'WORKING', 'keyless');",
                    expected: Expected::Rows {
                        columns: &[Column("rows_added", INT8), Column("rows_deleted", INT8)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --stat prints the statistics, which dolt_diff_stat() returns as a row")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: dolt_diff_ table",
            intent: "dolt_diff_keyless lists the working set's changes and the commit that added the rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (8,8);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = 9 WHERE c0 = 1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_c0, to_c1, from_c0, from_c1 FROM dolt_diff_keyless ORDER BY dolt_to_commit_date, to_c0 DESC, to_c1 DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("to_c0", INT4), Column("to_c1", INT4), Column("from_c0", INT4), Column("from_c1", INT4)],
                        rows: &[
                            &[T("2"), T("2"), Null, Null],
                            &[T("1"), T("1"), Null, Null],
                            &[T("1"), T("1"), Null, Null],
                            &[T("0"), T("0"), Null, Null],
                            &[Null, Null, T("1"), T("1")],
                            &[Null, Null, T("1"), T("1")],
                            &[Null, Null, T("0"), T("0")],
                            &[T("8"), T("8"), Null, Null],
                            &[T("1"), T("9"), Null, Null],
                            &[T("1"), T("9"), Null, Null],
                        ],
                        tag: "SELECT 10",
                    },
                    dolt: Some(Dolt::Differs("Postgres sorts NULLs last ascending and first descending, the reverse of MySQL, so the working set's rows, whose dolt_to_commit_date is NULL, follow the commit's, and the deleted rows, whose to_c0 is NULL, lead them. Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff column add/drop",
            intent: "The working set's diff shows a keyless table's added and dropped columns.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE keyless ADD COLUMN c2 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"unimplemented\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE keyless DROP COLUMN c0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_diff('HEAD', 'WORKING', 'keyless');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as a row")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff branches with identical mutation history",
            intent: "Branches that inserted the same keyless rows have no diff.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (7,7),(8,8),(9,9);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted on main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (7,7),(8,8),(9,9);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted on other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
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
            name: "keyless: diff deletes from two branches",
            intent: "Each branch's diff from main lists the keyless rows it deleted.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'deleted ones on right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("removed"), T("0"), T("0"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'deleted twos on left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("removed"), T("2"), T("2"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff duplicate deletes",
            intent: "Diffs list each deleted duplicate of a keyless row.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "CREATE TABLE dupe (c0 int, c1 int);",
                "INSERT INTO dupe (c0,c1) VALUES (1,1),(1,1),(1,1),(1,1),(1,1),(1,1),(1,1),(1,1),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'created table dupe');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dupe WHERE ctid IN (SELECT ctid FROM dupe LIMIT 2);",
                    expected: Expected::Tag("DELETE 2"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's DELETE ... LIMIT is a DELETE of the first rows a subquery picks by ctid")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'deleted two rows on right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'dupe') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dupe WHERE ctid IN (SELECT ctid FROM dupe LIMIT 4);",
                    expected: Expected::Tag("DELETE 4"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's DELETE ... LIMIT is a DELETE of the first rows a subquery picks by ctid")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'deleted four rows on left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'dupe') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff duplicate updates",
            intent: "Diffs list each updated duplicate of a keyless row as a deleted and an added row.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
                "CREATE TABLE dupe (c0 int, c1 int);",
                "INSERT INTO dupe (c0,c1) VALUES (1,1),(1,1),(1,1),(1,1),(1,1),(1,1),(1,1),(1,1),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'created table dupe');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dupe SET c1 = 2 WHERE ctid IN (SELECT ctid FROM dupe LIMIT 2);",
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's UPDATE ... LIMIT is an UPDATE of the first rows a subquery picks by ctid")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'updated two rows on right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'dupe') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("added"), Null, Null, T("1"), T("2")],
                            &[T("added"), Null, Null, T("1"), T("2")],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dupe SET c1 = 2 WHERE ctid IN (SELECT ctid FROM dupe LIMIT 4);",
                    expected: Expected::Tag("UPDATE 4"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's UPDATE ... LIMIT is an UPDATE of the first rows a subquery picks by ctid")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'updated four rows on left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'dupe') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("added"), Null, Null, T("1"), T("2")],
                            &[T("added"), Null, Null, T("1"), T("2")],
                            &[T("added"), Null, Null, T("1"), T("2")],
                            &[T("added"), Null, Null, T("1"), T("2")],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                            &[T("removed"), T("1"), T("1"), Null, Null],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: sql diff",
            intent: "The working set's diff as SQL deletes and inserts keyless rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"unimplemented\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 'keyless') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.keyless WHERE c0 = 2 AND c1 = 2;")],
                            &[T("INSERT INTO public.keyless (c0, c1) VALUES (3, 3);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'made changes');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = 13 WHERE c1 = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 'keyless') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.keyless WHERE c0 = 3 AND c1 = 3;")],
                            &[T("INSERT INTO public.keyless (c0, c1) VALUES (3, 13);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: sql diff as a patch",
            intent: "A branch's diff as SQL, run on the other branch, leaves no diff between them.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"unimplemented\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted values (3,3)');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('left', 'WORKING', 'keyless') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("INSERT INTO public.keyless (c0, c1) VALUES (3, 3);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('right', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
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
            name: "keyless: table replace",
            intent: "Replacing a keyless table's rows with the same rows leaves no diff, and with a new row lists it.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM keyless;",
                    expected: Expected::Tag("DELETE 4"),
                    dolt: Some(Dolt::Differs("dolt table import -r has no SQL form, so DELETE and INSERT replace the rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                    expected: Expected::Tag("INSERT 0 4"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so INSERT loads the CSV's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('HEAD', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM keyless;",
                    expected: Expected::Tag("DELETE 4"),
                    dolt: Some(Dolt::Differs("dolt table import -r has no SQL form, so DELETE and INSERT replace the rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (9,9),(0,0),(1,1),(1,1),(2,2);",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so INSERT loads the CSV's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('HEAD', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("added"), Null, Null, T("9"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff with in-place updates (working set)",
            intent: "An update of a keyless row diffs as the old row deleted and the new one added.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = 9 WHERE c0 = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('HEAD', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("added"), Null, Null, T("2"), T("9")],
                            &[T("removed"), T("2"), T("2"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: sql diff with in-place updates (working set)",
            intent: "An update of a keyless row diffs as SQL that deletes the old row and inserts the new one.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = 9 WHERE c0 = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"unimplemented\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 'keyless') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.keyless WHERE c0 = 2 AND c1 = 2;")],
                            &[T("INSERT INTO public.keyless (c0, c1) VALUES (2, 9);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: updates as a sql diff patch",
            intent: "Updates diffed as SQL and run on the other branch leave no diff between them.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = 9 WHERE c1 = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::Bug("Dolt's UPDATE (SET c1 = 22 WHERE c1 = 9) matches no row, where its commit message and expected patch change (2,2) to (2,9), so the port runs that change")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'updates (2,2) -> (2,9)');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('left', 'WORKING', 'keyless') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.keyless WHERE c0 = 2 AND c1 = 2;")],
                            &[T("INSERT INTO public.keyless (c0, c1) VALUES (2, 9);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM public.keyless WHERE c0 = 2 AND c1 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"unimplemented\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.keyless (c0, c1) VALUES (2, 9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('right', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
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
            name: "keyless: diff with in-place updates (branches)",
            intent: "Branches that updated the same keyless rows differently diff as each branch's rows deleted and added.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (7,7),(8,8),(9,9);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'added rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = c1+10 WHERE c0 > 6;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'updated on main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c1 = c1+20 WHERE c0 > 6;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'updated on other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("added"), Null, Null, T("7"), T("27")],
                            &[T("added"), Null, Null, T("8"), T("28")],
                            &[T("added"), Null, Null, T("9"), T("29")],
                            &[T("removed"), T("7"), T("17"), Null, Null],
                            &[T("removed"), T("8"), T("18"), Null, Null],
                            &[T("removed"), T("9"), T("19"), Null, Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff branches with reordered mutation history",
            intent: "Branches that inserted the same keyless rows in another order have no diff.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (7,7),(8,8),(9,9);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted on main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (9,9),(8,8),(7,7);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted on other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
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
            name: "keyless: diff branches with convergent mutation history",
            intent: "Branches whose keyless rows end the same by different changes have no diff.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (7,7),(8,8),(9,9);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted on main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (9,19),(8,8),(7,17);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c0 = 7, c1 = 7 WHERE c1 = 19;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c0 = 9, c1 = 9 WHERE c1 = 17;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted on other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
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
            name: "keyless: diff branches with offset mutation history",
            intent: "A branch with one more duplicate of a keyless row diffs as that row added.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (7,7),(8,8),(9,9);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted on main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (7,7),(7,7),(8,8),(9,9);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserted on other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("added"), Null, Null, T("7"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: diff delete+add against working",
            intent: "Deleting a keyless row and adding it back leaves no diff.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('HEAD', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
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
            name: "keyless: diff delete+add on two branches",
            intent: "A branch that deleted a keyless row and one that added a duplicate each diff from main by that row.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'deleted ones on right');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("removed"), T("2"), T("2"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'deleted twos on left');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_c0, from_c1, to_c0, to_c1 FROM dolt_diff('main', 'WORKING', 'keyless') ORDER BY dolt_diff_type, from_c0, from_c1, to_c0, to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_c0", INT4), Column("from_c1", INT4), Column("to_c0", INT4), Column("to_c1", INT4)],
                        rows: &[
                            &[T("added"), Null, Null, T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: create secondary index",
            intent: "A secondary index on a keyless table finds its rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON keyless (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'keyless';",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx"), T("CREATE INDEX idx ON public.keyless USING btree (c1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW INDEX is MySQL's, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless WHERE c1 > 0 ORDER BY c0;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0 FROM keyless WHERE c1 = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: secondary index insert",
            intent: "A secondary index on a keyless table finds inserted rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON keyless (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (3,3), (4,4);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0 FROM keyless WHERE c1 = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: secondary index duplicate insert",
            intent: "A secondary index on a keyless table finds each inserted duplicate.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON keyless (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (3,3), (4,4), (4,4);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0 FROM keyless WHERE c1 = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: secondary index update",
            intent: "A secondary index on a keyless table follows updated rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON keyless (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE keyless SET c0 = c0 + 1;",
                    expected: Expected::Tag("UPDATE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless ORDER BY c0;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0 FROM keyless WHERE c1 = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: secondary index delete single",
            intent: "A secondary index on a keyless table drops a deleted row.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON keyless (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (3,3), (4,4);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 4;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0 FROM keyless WHERE c1 > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0 FROM keyless WHERE c0 > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: secondary index delete duplicate",
            intent: "A secondary index on a keyless table drops every deleted duplicate.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON keyless (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (3,3), (4,4), (4,4);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM keyless WHERE c0 = 4;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0 FROM keyless WHERE c1 > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c0 FROM keyless WHERE c0 > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: check constraint violation rolls back",
            intent: "An insert that violates a check constraint inserts none of its rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (i int CHECK (i < 10));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (100);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"test\" violates check constraint \"test_i_check\"", detail: "Failing row contains (100).", schema: "public", table: "test", constraint: "test_i_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2), (3), (100), (4);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"test\" violates check constraint \"test_i_check\"", detail: "Failing row contains (100).", schema: "public", table: "test", constraint: "test_i_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
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
            name: "keyless: inserting invalid values are rolled back",
            intent: "An insert with a value of the wrong type inserts none of its rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (i int CHECK (i < 10));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES ('thisisastring');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"thisisastring\"", position: 26, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2), (3), ('thisisastring'), (4);",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"thisisastring\"", position: 36, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
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
            name: "keyless: insert into keyless table with unique index",
            intent: "A unique index on a keyless table refuses a duplicate, and a failing insert inserts none of its rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable (pk int UNIQUE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1),(2),(3),(4);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"mytable_pk_key\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "mytable", constraint: "mytable_pk_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (500000), (5000001), (3);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"mytable_pk_key\"", detail: "Key (pk)=(3) already exists.", schema: "public", table: "mytable", constraint: "mytable_pk_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mytable WHERE pk IN (500000,5000001);",
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
                    query: "SELECT count(*) FROM mytable;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM mytable ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns reads what the index holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: insert into keyless table with unique index and auto increment",
            intent: "A unique identity column of a keyless table refuses a duplicate and generates the next key.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE gis (pk int UNIQUE NOT NULL GENERATED BY DEFAULT AS IDENTITY, shape point NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's GEOMETRY and POINT() are Postgres' point type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO gis VALUES (1, point(1,1));",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's GEOMETRY and POINT() are Postgres' point type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO gis VALUES (1, point(1,1));",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"gis_pk_key\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "gis", constraint: "gis_pk_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM gis WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO gis VALUES (DEFAULT, point(1,1));",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"gis_pk_key\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "gis", constraint: "gis_pk_key", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's NULL in an AUTO_INCREMENT column asks for the next key, which DEFAULT does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM gis WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: string type unique key index",
            intent: "A unique index on a string column of a keyless table refuses a duplicate.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable (pk int, val varchar(6) UNIQUE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 'nekter');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 'nekter');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"mytable_val_key\"", detail: "Key (val)=(nekter) already exists.", schema: "public", table: "mytable", constraint: "mytable_val_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mytable WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM mytable ORDER BY val;",
                    expected: Expected::Rows {
                        columns: &[Column("val", VARCHAR)],
                        rows: &[
                            &[T("nekter")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns reads what the index holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: compound unique key index",
            intent: "A unique constraint on two columns of a keyless table refuses a duplicate pair.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable (pk int, v1 int, v2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mytable ADD CONSTRAINT ux UNIQUE (v1, v2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"ux\"", detail: "Key (v1, v2)=(2, 2) already exists.", schema: "public", table: "mytable", constraint: "ux", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) AS count FROM mytable WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, v2 FROM mytable ORDER BY v1, v2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns reads what the index holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: replace into and unique key index",
            intent: "REPLACE INTO replaces the row a unique constraint matches.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable (pk int, v1 int, v2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"Keyless tables with unique indexes do not properly support replace into semantics\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mytable ADD CONSTRAINT ux UNIQUE (v1, v2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 2, 2) ON CONFLICT (v1, v2) DO UPDATE SET pk = excluded.pk;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's REPLACE INTO is an INSERT ... ON CONFLICT DO UPDATE")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: batch import with keyless unique index",
            intent: "A unique constraint on a keyless table refuses a duplicate and keeps the other rows.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable (pk int, v1 int, v2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mytable ADD CONSTRAINT ux UNIQUE (v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (3, 3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (2, 2, 3);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"ux\"", detail: "Key (v1)=(2) already exists.", schema: "public", table: "mytable", constraint: "ux", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("2")],
                            &[T("3"), T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable WHERE v1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1 FROM mytable ORDER BY v1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns reads what the index holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: batch import with keyless unique index and secondary index",
            intent: "A unique constraint and a secondary index on a keyless table stay consistent after a refused duplicate.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable (pk int, v1 int, v2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mytable ADD CONSTRAINT ux UNIQUE (v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX myidx ON mytable (v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (3, 3, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (2, 2, 3);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"ux\"", detail: "Key (v1)=(2) already exists.", schema: "public", table: "mytable", constraint: "ux", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("2")],
                            &[T("3"), T("3"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable WHERE v1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1 FROM mytable ORDER BY v1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2 FROM mytable ORDER BY v2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns reads what the index holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless: batch import into the unique key correctly works",
            intent: "Loading rows that match a unique key's rows updates them.",
            set_up_script: &[
                "CREATE TABLE keyless (c0 int, c1 int);",
                "INSERT INTO keyless VALUES (0,0),(2,2),(1,1),(1,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable (pk int, v1 int, v2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"index error handling does not work with bulk import\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mytable ADD CONSTRAINT ux UNIQUE (v1, v2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1, 1, 1), (2, 2, 2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (3, 1, 1), (4, 2, 2) ON CONFLICT (v1, v2) DO UPDATE SET pk = excluded.pk;",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::Differs("dolt table import -u has no SQL form, so INSERT ... ON CONFLICT DO UPDATE loads the CSV's rows")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
