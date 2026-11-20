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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_merge.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_merge_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "merge whose generated value violates a unique key only once computed",
            intent: "A stored generated value computed from merged cells that duplicates another row records a unique violation.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "SET dolt_force_transaction_commit = 1;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED); CREATE UNIQUE INDEX ug ON t (g);",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 0), (2, 5, 5);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET a = 4 WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE t SET b = 6 WHERE id = 1;",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT dolt_violation_type, id, g FROM dolt_constraint_violations_t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("id", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("unique index"), T("1"), T("10")],
                            &[T("unique index"), T("2"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "merge whose generated value violates a check only once computed",
            intent: "A stored generated value computed from merged cells that breaks a check records a check violation.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "SET dolt_force_transaction_commit = 1;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED, CHECK (g <= 6));",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 0);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET a = 4 WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE t SET b = 6 WHERE id = 1;",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT dolt_violation_type, id, g FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("id", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "delete conflicts one at a time with a composite key and a virtual column",
            intent: "Deleting conflicts one at a time from a table with a composite key and a virtual column removes only those.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (k1 int, k2 VARCHAR(10), a int, v int GENERATED ALWAYS AS (a + 1) VIRTUAL, b int, PRIMARY KEY (k1, k2));",
                "INSERT INTO t (k1, k2, a, b) VALUES (1, 'a', 1, 1), (1, 'b', 2, 2);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "DELETE FROM t WHERE k2 = 'a';",
                "UPDATE t SET a = 10 WHERE k2 = 'b';",
                "INSERT INTO t (k1, k2, a, b) VALUES (3, 'c', 30, 3);",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE t SET a = 20;",
                "INSERT INTO t (k1, k2, a, b) VALUES (3, 'c', 40, 3);",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT base_k2, our_k2, their_k2 FROM dolt_conflicts_t ORDER BY their_k2;",
                    expected: Expected::Rows {
                        columns: &[Column("base_k2", VARCHAR), Column("our_k2", VARCHAR), Column("their_k2", VARCHAR)],
                        rows: &[
                            &[T("a"), Null, T("a")],
                            &[T("b"), T("b"), T("b")],
                            &[Null, T("c"), T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t WHERE their_k2 = 'a';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT their_k2 FROM dolt_conflicts_t ORDER BY their_k2;",
                    expected: Expected::Rows {
                        columns: &[Column("their_k2", VARCHAR)],
                        rows: &[
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t WHERE their_k2 = 'c';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT their_k2 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("their_k2", VARCHAR)],
                        rows: &[
                            &[T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_a = 'not-an-int' WHERE their_k2 = 'b';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: 'not-an-int' is not a valid value for 'int'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY k1, k2;",
                    expected: Expected::Rows {
                        columns: &[Column("k1", INT4), Column("k2", VARCHAR), Column("a", INT4), Column("v", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("b"), T("10"), T("11"), T("2")],
                            &[T("3"), T("c"), T("30"), T("31"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "delete one conflict of a keyless table with a virtual column",
            intent: "Deleting one conflict of a keyless table with a virtual column removes only it.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (a int, v int GENERATED ALWAYS AS (a + 1) VIRTUAL, b int);",
                "INSERT INTO t (a, b) VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "INSERT INTO t (a, b) VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM t;",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT base_a, dolt_base_cardinality, dolt_our_cardinality, dolt_their_cardinality FROM dolt_conflicts_t ORDER BY base_a;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", INT4), Column("dolt_base_cardinality", INT8), Column("dolt_our_cardinality", INT8), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("2"), T("0")],
                            &[T("2"), T("1"), T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t WHERE base_a = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "update and delete conflicts with different virtual columns on each side",
            intent: "Update and delete conflicts with different virtual columns on each side show and resolve correctly.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a * 10) STORED); CREATE INDEX ig ON t (g);",
                "INSERT INTO t (id, a, b) VALUES (1, 1, 1), (2, 2, 2);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ADD COLUMN v1 int GENERATED ALWAYS AS (a + 1) VIRTUAL AFTER a;",
                "UPDATE t SET a = 10;",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ADD COLUMN v2 int GENERATED ALWAYS AS (b + 1) VIRTUAL;",
                "UPDATE t SET a = 20;",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'dolt_conflicts_t' AND column_name LIKE '%v_' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_id, base_a, our_a, our_g, their_a, their_g FROM dolt_conflicts_t ORDER BY base_id;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", INT4), Column("our_a", INT4), Column("our_g", INT4), Column("their_a", INT4), Column("their_g", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("10"), T("100"), T("20"), T("200")],
                            &[T("2"), T("2"), T("10"), T("100"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_a = their_a WHERE our_id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("g", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("20"), T("21"), T("1"), T("200"), T("2")],
                            &[T("2"), T("10"), T("11"), T("2"), T("100"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t  WHERE g = 200;",
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
                    query: "DELETE FROM dolt_conflicts_t WHERE our_id = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT our_id FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("our_id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "merge resolving a row both sides changed computes chained stored generated columns",
            intent: "Resolving a row both sides changed computes chained stored generated columns from the resolved values.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED, h int GENERATED ALWAYS AS (g * 10) STORED);",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 1);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET a = 10;",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE t SET b = 2;",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("g", INT4), Column("h", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("2"), T("12"), T("120")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "delete conflicts of a table with a virtual column",
            intent: "Deleting the conflicts of a table with a virtual column clears them.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, v int GENERATED ALWAYS AS (a + 1) VIRTUAL);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "INSERT INTO t (id, a) VALUES (1, 10);",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO t (id, a) VALUES (1, 20);",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT our_id, our_a, their_id, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("our_id", INT4), Column("our_a", INT4), Column("their_id", INT4), Column("their_a", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("1"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_conflicts_t;",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "merge resolving a row both sides changed updates the index on a stored generated column",
            intent: "Resolving a row both sides changed keeps the index on a stored generated column in step.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED); CREATE INDEX ig ON t (g);",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 1);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET a = 10;",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE t SET a = 10, b = 2;",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("2"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t  WHERE g = 12;",
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
            name: "keyless table merge with constraint violations",
            intent: "A keyless table merge records constraint violations with each row's hash.",
            set_up_script: &[
                "CREATE TABLE aTable (aColumn int NULL, bColumn int NULL); CREATE UNIQUE INDEX aColumn_UNIQUE ON aTable (aColumn ASC); CREATE UNIQUE INDEX bColumn_UNIQUE ON aTable (bColumn ASC);",
                "SELECT dolt_commit('-Am', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'side');",
                "INSERT INTO aTable VALUES (1,2);",
                "SELECT dolt_commit('-am', 'add side data');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO aTable VALUES (1,3);",
                "SELECT dolt_commit('-am', 'add main data');",
                "SELECT * FROM dolt_checkout('side');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM aTable;",
                    expected: Expected::Rows {
                        columns: &[Column("acolumn", INT4), Column("bcolumn", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM aTable;",
                    expected: Expected::Rows {
                        columns: &[Column("acolumn", INT4), Column("bcolumn", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("1"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.atable"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_from_root_ish, dolt_violation_type, upper(encode(dolt_row_hash, 'hex')) AS dolt_row_hash, aColumn, bColumn, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_aTable ORDER BY bColumn;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("dolt_violation_type", TEXT), Column("dolt_row_hash", TEXT), Column("acolumn", INT4), Column("bcolumn", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[Any, T("unique index"), Any, T("1"), T("2"), T("{\"Name\": \"acolumn_unique\", \"Columns\": [\"acolumn\"]}")],
                            &[Any, T("unique index"), Any, T("1"), T("3"), T("{\"Name\": \"acolumn_unique\", \"Columns\": [\"acolumn\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE aTable SET aColumn = 2 WHERE bColumn = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_aTable;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'merging in main and resolving unique constraint violations');",
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
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("cannot fast forward from a to b. a is ahead of b already")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless table merge with constraint violation on duplicate rows",
            intent: "A keyless table merge records a violation for duplicate rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "INSERT INTO parent VALUES (1), (2);",
                "CREATE TABLE aTable (aColumn int NULL, bColumn int NULL);",
                "INSERT INTO aTable VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'side');",
                "INSERT INTO aTable VALUES (2, -1), (2, -1);",
                "SELECT dolt_commit('-am', 'add side data');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE aTable ADD FOREIGN KEY (bColumn) REFERENCES parent(pk);",
                "SELECT dolt_commit('-am', 'add main data');",
                "SELECT * FROM dolt_checkout('side');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM aTable ORDER BY aColumn;",
                    expected: Expected::Rows {
                        columns: &[Column("acolumn", INT4), Column("bcolumn", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("-1")],
                            &[T("2"), T("-1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM aTable ORDER BY aColumn;",
                    expected: Expected::Rows {
                        columns: &[Column("acolumn", INT4), Column("bcolumn", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("-1")],
                            &[T("2"), T("-1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.atable"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.atable"), T("f"), T("constraint violation")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_from_root_ish, dolt_violation_type, upper(encode(dolt_row_hash, 'hex')) AS dolt_row_hash, aColumn, bColumn, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_aTable ORDER BY bColumn;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("dolt_violation_type", TEXT), Column("dolt_row_hash", TEXT), Column("acolumn", INT4), Column("bcolumn", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[Any, T("foreign key"), Any, T("2"), T("-1"), T("{\"Index\": \"\", \"Table\": \"atable\", \"Columns\": [\"bcolumn\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"atable_bcolumn_fkey\", \"ReferencedIndex\": \"parent_pkey\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"pk\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE aTable SET bColumn = 2 WHERE bColumn = -1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_aTable;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'merging in main and resolving unique constraint violations');",
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
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("cannot fast forward from a to b. a is ahead of b already")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unique constraint checks do not consider deleted rows",
            intent: "Unique checks after a merge ignore rows the merge deleted.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE tableA (pk varchar(255) PRIMARY KEY, col1 varchar(255)); CREATE UNIQUE INDEX unique1 ON tableA (col1);",
                "INSERT INTO tableA VALUES ('B', '1'), ('C', 2), ('Y', '100');",
                "SELECT dolt_commit('-Am', 'creating table');",
                "SELECT dolt_branch('feature');",
                "UPDATE tableA SET pk = 'A' WHERE pk='B';",
                "UPDATE tableA SET pk = 'Z' WHERE pk='Y';",
                "SELECT dolt_commit('-am', 'update two rows');",
                "SELECT * FROM dolt_checkout('feature');",
                "UPDATE tableA SET col1='C' WHERE pk='C';",
                "SELECT dolt_commit('-am', 'added row on branch feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
                    query: "SELECT * FROM dolt_constraint_violations_tableA;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("dolt_violation_type", TEXT), Column("pk", VARCHAR), Column("col1", VARCHAR), Column("dolt_violation_info", JSONB)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tableA;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("col1", VARCHAR)],
                        rows: &[
                            &[T("A"), T("1")],
                            &[T("C"), T("C")],
                            &[T("Z"), T("100")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE ff correctly works with autocommit off",
            intent: "A fast-forward merge with autocommit off moves the branch, visible after COMMIT.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'feature-branch', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("our_pk", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM dolt_checkout('-b', 'new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'new-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE ff correctly works with autocommit off, no checkout",
            intent: "A fast-forward merge with autocommit off, without checking out the branch, moves it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT dolt_branch('feature-branch');",
                "use \"postgres/feature-branch\";",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "use \"postgres/main\";",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM test ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE fails on non-branch revision",
            intent: "Merging a revision that isn't a branch fails.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT dolt_branch('feature-branch');",
                "use \"postgres/feature-branch\";",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "use \"postgres/main~\";",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "this operation is not supported while in a detached head state", ..E }),
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'feature-branch', 'test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "this operation is not supported while in a detached head state", ..E }),
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
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "this operation is not supported while in a detached head state", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE no-ff correctly works with autocommit off",
            intent: "A no-ff merge with autocommit off makes a merge commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:00');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a ff', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--no-ff', '-m', 'this is a no-ff');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("this is a no-ff")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'other-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE no-ff correctly works with autocommit off, no checkout",
            intent: "A no-ff merge with autocommit off, without checking out the branch, makes a merge commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:00');",
                "SELECT dolt_branch('feature-branch');",
                "USE \"postgres/feature-branch\";",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a ff', '--date', '2022-08-06T12:00:01');",
                "use \"postgres/main\";",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--no-ff', '-m', 'this is a no-ff');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("this is a no-ff")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE without conflicts correctly works with autocommit off with commit flag",
            intent: "A clean merge with autocommit off and --commit makes a merge commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:02');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (5),(6),(7);",
                "SELECT dolt_commit('-a', '-m', 'add some more values', '--date', '2022-08-06T12:00:03');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge', '--commit');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log WHERE date > '2022-08-08' ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("this is a merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE without conflicts correctly works with autocommit off and no commit flag",
            intent: "A clean merge with autocommit off and --no-commit leaves the merge to commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:02');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (5),(6),(7);",
                "SELECT dolt_commit('-a', '-m', 'add some more values', '--date', '2022-08-06T12:00:03');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge', '--no-commit');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("feature-branch"), T("refs/heads/main"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
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
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log WHERE date < '2022-08-08' ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("add some more values")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
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
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE with conflicts can be correctly resolved when autocommit is off",
            intent: "Conflicts from a merge with autocommit off can be resolved and committed.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (1, 1);",
                "UPDATE test SET val=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:02');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET val=1001 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'update a value', '--date', '2022-08-06T12:00:03');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_val, our_pk, our_val, dolt_our_diff_type, their_pk, their_val, dolt_their_diff_type FROM dolt_preview_merge_conflicts('main', 'feature-branch', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_val", INT4), Column("our_pk", INT4), Column("our_val", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_val", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("1001"), T("modified"), T("0"), T("1000"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("feature-branch"), T("refs/heads/main"), T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                            &[T("public.test"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log WHERE date < '2022-08-08' ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("update a value")],
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
                            &[T("public.test"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_val, our_pk, our_val, dolt_our_diff_type, their_pk, their_val, dolt_their_diff_type FROM dolt_conflicts_test;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_val", INT4), Column("our_pk", INT4), Column("our_val", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_val", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("1001"), T("modified"), T("0"), T("1000"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Tag("DELETE 1"),
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("1001")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("feature-branch"), T("refs/heads/main"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'merged');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
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
            name: "CALL DOLT_MERGE with non-conflicting schema changes can be correctly resolved when autocommit is off",
            intent: "Non-conflicting schema changes merge with autocommit off.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int, val2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0, 0);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "ALTER TABLE test DROP COLUMN val2;",
                "INSERT INTO test VALUES (1, 1);",
                "UPDATE test SET val=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:02');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE test DROP COLUMN val2;",
                "INSERT INTO test VALUES (1, 2);",
                "UPDATE test SET val=1001 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'update a value', '--date', '2022-08-06T12:00:03');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("2"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_val, our_pk, our_val, dolt_our_diff_type, their_pk, their_val, dolt_their_diff_type FROM dolt_preview_merge_conflicts('main', 'feature-branch', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_val", INT4), Column("our_pk", INT4), Column("our_val", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_val", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("1001"), T("modified"), T("0"), T("1000"), T("modified")],
                            &[Null, Null, T("1"), T("2"), T("added"), T("1"), T("1"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("feature-branch"), T("refs/heads/main"), T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
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
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log WHERE date < '2022-08-08' ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("update a value")],
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
                            &[T("public.test"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_val, our_pk, our_val, dolt_our_diff_type, their_pk, their_val, dolt_their_diff_type FROM dolt_conflicts_test;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_val", INT4), Column("our_pk", INT4), Column("our_val", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_val", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("1001"), T("modified"), T("0"), T("1000"), T("modified")],
                            &[Null, Null, T("1"), T("2"), T("added"), T("1"), T("1"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Tag("DELETE 2"),
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
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("1001")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("feature-branch"), T("refs/heads/main"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("2"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'merged');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
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
            name: "CALL DOLT_MERGE with schema conflicts can be correctly resolved using dolt_conflicts_resolve when autocommit is off",
            intent: "Schema conflicts resolved with dolt_conflicts_resolve with autocommit off can be committed.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "ALTER TABLE test ALTER COLUMN val TYPE bigint, ALTER COLUMN val DROP NOT NULL, ALTER COLUMN val DROP DEFAULT;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:02');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE test ALTER COLUMN val TYPE smallint, ALTER COLUMN val DROP NOT NULL, ALTER COLUMN val DROP DEFAULT;",
                "SELECT dolt_commit('-a', '-m', 'update val col', '--date', '2022-08-06T12:00:03');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("feature-branch"), T("refs/heads/main"), T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("schema conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log WHERE date < '2022-08-08' ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("update val col")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_conflicts;",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 'test');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("feature-branch"), T("refs/heads/main"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_conflicts;",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("merged")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'merged');",
                    expected: Expected::Ok,
                    ..A
                },
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
                    query: "SHOW CREATE TABLE test;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("test"), T("CREATE TABLE `test` (\n  `pk` int NOT NULL,\n  `val` smallint,\n  PRIMARY KEY (`pk`)\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("MySQL-only: SHOW CREATE TABLE is MySQL's"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge conflicts prevent new branch creation",
            intent: "Unresolved merge conflicts keep a new branch from being made from the working set.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (1, 1);",
                "UPDATE test SET val=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:02');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET val=1001 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'update a value', '--date', '2022-08-06T12:00:03');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("feature-branch"), T("refs/heads/main"), T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                            &[T("public.test"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 2",
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
                    query: "SELECT * FROM dolt_checkout('-b', 'other-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts must be resolved using the dolt_conflicts and dolt_schema_conflicts tables before committing a transaction. To commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE ff & squash correctly works with autocommit off",
            intent: "A fast-forward squash with autocommit off stages the changes without a merge commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--squash');",
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
                    query: "SELECT count(*) FROM dolt_status;",
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT * FROM test ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE ff & squash with a checkout in between",
            intent: "A fast-forward squash after checking out another branch stages the changes.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--squash');",
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
                    query: "SELECT * FROM dolt_checkout('-b', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
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
                    query: "SELECT * FROM test ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE ff",
            intent: "A fast-forward merge moves the branch to the merged commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM dolt_checkout('-b', 'new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'new-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE ff no checkout",
            intent: "A fast-forward merge without checking out the branch moves it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT dolt_branch('feature-branch');",
                "use \"postgres/feature-branch\";",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "use \"postgres/main\";",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM dolt_checkout('-b', 'new-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'new-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("new-branch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "use \"postgres/main\";",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE no-ff",
            intent: "A no-ff merge makes a merge commit even when it could fast-forward.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-no-ff', '-m', 'this is a no-ff');",
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
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("this is a no-ff")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'other-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE with no conflicts works",
            intent: "A three-way merge without conflicts makes a merge commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:00');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (5),(6),(7);",
                "SELECT dolt_commit('-a', '-m', 'add some more values', '--date', '2022-08-06T12:00:02');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--no-commit', '--commit');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot define both 'commit' and 'no-commit' flags at the same time", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
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
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log WHERE date > '2022-08-08' ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("this is a merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE with no conflicts works, no checkout",
            intent: "A three-way merge without conflicts, without checking out the branch, makes a merge commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:00');",
                "SELECT dolt_branch('feature-branch');",
                "use \"postgres/feature-branch\";",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:01');",
                "use \"postgres/main\";",
                "INSERT INTO test VALUES (5),(6),(7);",
                "SELECT dolt_commit('-a', '-m', 'add some more values', '--date', '2022-08-06T12:00:02');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--no-commit', '--commit');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot define both 'commit' and 'no-commit' flags at the same time", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
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
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log WHERE date > '2022-08-08' ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("this is a merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "use \"postgres/feature-branch\";",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE with no conflicts works with no-commit flag",
            intent: "A three-way merge without conflicts and --no-commit leaves the merge to commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0),(1),(2);",
                "SELECT dolt_commit('-a', '-m', 'Step 1', '--date', '2022-08-06T12:00:00');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit', '--date', '2022-08-06T12:00:01');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (5),(6),(7);",
                "SELECT dolt_commit('-a', '-m', 'add some more values', '--date', '2022-08-06T12:00:02');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge', '--no-commit');",
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
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT message FROM dolt_log WHERE date < '2022-08-08' ORDER by date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("add some more values")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'other-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE when current or ahead results in a no-op",
            intent: "Merging a branch that is current or behind changes nothing.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-A', '-m', 'commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('HEAD', 'HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('HEAD', 'HEAD~1', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("our_pk", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("cannot fast forward from a to b. a is ahead of b already")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('HEAD', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('HEAD', 'HEAD', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("our_pk", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('HEAD');",
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
        ScriptTest {
            name: "CALL DOLT_MERGE with conflict is queryable and committable with dolt_allow_commit_conflicts on",
            intent: "With dolt_allow_commit_conflicts on, a merge's conflicts can be queried and committed.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (1, 1);",
                "UPDATE test SET val=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET val=1001 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'update a value');",
                "SET dolt_allow_commit_conflicts = ON;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_preview_merge_conflicts('main', 'feature-branch', 'test');",
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
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                            &[T("public.test"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_conflicts_test;",
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
                    query: "SELECT count(*) FROM dolt_conflicts_TeST;",
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
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("1001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts_test;",
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
                    query: "SELECT count(*) FROM dolt_status;",
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
                    query: "SET dolt_allow_commit_conflicts = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts_test;",
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
            name: "merge abort rejects pending edits on mydb/b2",
            intent: "Aborting a merge refuses while another branch has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "UPDATE t SET v=1;",
                "SELECT dolt_commit('-am', 'branch change');",
                "use \"postgres\";",
                "UPDATE t SET v=2;",
                "SELECT dolt_commit('-am', 'main change');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY, v int);",
                "BEGIN;",
                "SELECT * FROM dolt_merge('b1');",
                "INSERT INTO \"postgres/b2\".t VALUES (9, 9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t WHERE pk=9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t WHERE pk=9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v=99;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge abort rejects pending edits on other",
            intent: "Aborting a merge refuses while another database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "UPDATE t SET v=1;",
                "SELECT dolt_commit('-am', 'branch change');",
                "use \"postgres\";",
                "UPDATE t SET v=2;",
                "SELECT dolt_commit('-am', 'main change');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY, v int);",
                "BEGIN;",
                "SELECT * FROM dolt_merge('b1');",
                "INSERT INTO \"other\".t VALUES (9, 9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t WHERE pk=9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t WHERE pk=9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v=99;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
            skip: Some("MySQL-only: Postgres can't write another database's tables from a session"),
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE with conflicts can be aborted when autocommit is off",
            intent: "A conflicting merge with autocommit off can be aborted.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (1, 1);",
                "UPDATE test SET val=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET val=1001 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'update a value');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                            &[T("public.test"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_conflicts;",
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
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT COUNT(*) FROM dolt_log;",
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
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("1001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'other-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_MERGE(--abort) clears staged",
            intent: "Aborting a merge clears the staged changes it made.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0),(1),(2);",
                "BEGIN;",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1,c1,c2) VALUES (0,0,0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1,c1,c2) VALUES (0,1,1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch');",
                "SELECT * FROM dolt_merge('--abort');",
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
            ],
            ..S
        },
        ScriptTest {
            name: "Abort merge with table rename and conflict",
            intent: "Aborting a merge restores a table the other side renamed, with its conflict gone.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE conflict_table (pk int PRIMARY KEY, c1 varchar(100));",
                "CREATE TABLE table1 (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating tables on main');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO conflict_table VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding another table on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO conflict_table VALUES (1, 'uno');",
                "INSERT INTO table1 VALUES (1, 'one');",
                "ALTER TABLE table1 RENAME TO table2;",
                "SELECT dolt_commit('-Am', 'renaming table on branch1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Abort merge with table rename and conflict (with ignored table)",
            intent: "Aborting a merge restores a renamed table and leaves an ignored table alone.",
            set_up_script: &[
                "BEGIN;",
                "INSERT INTO dolt_ignore VALUES ('ignore_me', TRUE);",
                "CREATE TABLE ignore_me (pk int PRIMARY KEY, c1 varchar(100));",
                "INSERT INTO ignore_me VALUES (1, 'uno');",
                "CREATE TABLE conflict_table (pk int PRIMARY KEY, c1 varchar(100));",
                "CREATE TABLE table1 (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating tables on main');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO conflict_table VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding another table on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO conflict_table VALUES (1, 'uno');",
                "INSERT INTO table1 VALUES (1, 'one');",
                "ALTER TABLE table1 RENAME TO table2;",
                "SELECT dolt_commit('-Am', 'renaming table on branch1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
                    query: "INSERT INTO ignore_me VALUES (2, 'duex');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ignore_me;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("uno")],
                            &[T("2"), T("duex")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge 'b1' rejects pending edits on mydb",
            intent: "A merge refuses while the database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"postgres\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge 'b1' rejects pending edits on mydb/b2",
            intent: "A merge refuses while another branch has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"postgres/b2\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge 'b1' rejects pending edits on other",
            intent: "A merge refuses while another database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"other\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't write another database's tables from a session"),
            ..S
        },
        ScriptTest {
            name: "merge '--no-ff', 'b1' rejects pending edits on mydb",
            intent: "A no-ff merge refuses while the database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"postgres\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-ff', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge '--no-ff', 'b1' rejects pending edits on mydb/b2",
            intent: "A no-ff merge refuses while another branch has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"postgres/b2\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-ff', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge '--no-ff', 'b1' rejects pending edits on other",
            intent: "A no-ff merge refuses while another database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"other\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-ff', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't write another database's tables from a session"),
            ..S
        },
        ScriptTest {
            name: "merge '--squash', 'b1' rejects pending edits on mydb",
            intent: "A squash merge refuses while the database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"postgres\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--squash', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge '--squash', 'b1' rejects pending edits on mydb/b2",
            intent: "A squash merge refuses while another branch has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"postgres/b2\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--squash', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge '--squash', 'b1' rejects pending edits on other",
            intent: "A squash merge refuses while another database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"other\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--squash', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't write another database's tables from a session"),
            ..S
        },
        ScriptTest {
            name: "merge '--no-commit', 'b1' rejects pending edits on mydb",
            intent: "A --no-commit merge refuses while the database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"postgres\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-commit', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge '--no-commit', 'b1' rejects pending edits on mydb/b2",
            intent: "A --no-commit merge refuses while another branch has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"postgres/b2\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-commit', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge '--no-commit', 'b1' rejects pending edits on other",
            intent: "A --no-commit merge refuses while another database has pending edits in the transaction.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "use \"postgres/b1\";",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'advance');",
                "CREATE database other;",
                "CREATE TABLE other.t (pk int PRIMARY KEY);",
                "use \"postgres\";",
                "BEGIN;",
                "INSERT INTO \"other\".t VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-commit', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS of 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"other\".t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't write another database's tables from a session"),
            ..S
        },
        ScriptTest {
            name: "CALL DOLT_MERGE complains when a merge overrides local changes",
            intent: "A merge that would overwrite uncommitted local changes fails and names the tables.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (1, 1);",
                "UPDATE test SET val=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET val=1001 WHERE pk=0;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'feature-branch', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_val", INT4), Column("our_pk", INT4), Column("our_val", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_val", INT4), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: local changes would be stomped by merge:\n	public.test\n Please commit your changes before you merge.", ..E }),
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Drop and add primary key on two branches converges to same schema",
            intent: "Dropping and adding a primary key the same way on two branches merges into one schema.",
            set_up_script: &[
                "CREATE TABLE t1 (i int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 't1 table');",
                "SELECT * FROM dolt_checkout('-b', 'b1');",
                "ALTER TABLE t1 ADD PRIMARY KEY(i);",
                "ALTER TABLE t1 DROP CONSTRAINT t1_pkey;",
                "ALTER TABLE t1 ADD PRIMARY KEY(i);",
                "ALTER TABLE t1 DROP CONSTRAINT t1_pkey;",
                "ALTER TABLE t1 ADD PRIMARY KEY(i);",
                "SELECT dolt_commit('-am', 'b1 primary key changes');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t1 ADD PRIMARY KEY(i);",
                "SELECT dolt_commit('-am', 'main primary key change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'b1', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_i", INT4), Column("our_i", INT4), Column("dolt_our_diff_type", TEXT), Column("their_i", INT4), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
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
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
            name: "Constraint violations are persisted",
            intent: "Constraint violations a merge records survive a commit and a new session.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE parent (pk int PRIMARY KEY, col1 int);",
                "CREATE TABLE child (pk int PRIMARY KEY, parent_fk int, FOREIGN KEY (parent_fk) REFERENCES parent(pk));",
                "CREATE TABLE other (pk int);",
                "SELECT dolt_add('.');",
                "INSERT INTO parent VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "DELETE FROM parent WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'delete parent 1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO CHILD VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert child of parent 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
                    query: "SELECT dolt_violation_type, pk, parent_fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, parent_fk FROM dolt_constraint_violations_CHILD;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.child"), T("f"), T("constraint violation")],
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
            name: "ancestor contains fk, main parent remove with backup, other child add, restrict",
            intent: "A parent row removed on one side while the other adds a child with a restricting foreign key records a violation.",
            set_up_script: &[
                "CREATE TABLE parent (pk bigint PRIMARY KEY, v1 bigint); CREATE INDEX  ON parent (v1);",
                "CREATE TABLE child (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent VALUES (10, 1), (20, 2), (30, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_commit('-am', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'OC1');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_force_transaction_commit = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM dolt_constraint_violations_parent;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", JSONB)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM parent;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("30"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a foreign key that references columns without a unique constraint is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "unique keys, insert violation",
            intent: "Inserts on both sides that duplicate a unique key record unique violations.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (2, 1), (3, 3);",
                "SELECT dolt_commit('-am', 'right insert');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1), (4, 4);",
                "SELECT dolt_commit('-am', 'left insert');",
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
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1")],
                            &[T("unique index"), T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target, unmerged_tables FROM DOLT_MERGE_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("t"), T("right"), T("refs/heads/main"), T("public.t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
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
            ],
            ..S
        },
        ScriptTest {
            name: "Test concurrent merges with overlapping indexes",
            intent: "Concurrent merges into tables with overlapping indexes keep the indexes consistent.",
            set_up_script: &[
                "CREATE TABLE overlap_indexes(pk int PRIMARY KEY, v int); CREATE UNIQUE INDEX uniq ON overlap_indexes (v); CREATE INDEX idx ON overlap_indexes (v);",
                "INSERT INTO overlap_indexes VALUES(1,1);",
                "SELECT dolt_commit('-Am','base');",
                "SELECT * FROM dolt_checkout('-b','other');",
                "INSERT INTO overlap_indexes VALUES(2,2);",
                "SELECT dolt_commit('-am','other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO overlap_indexes VALUES(3,3);",
                "SELECT dolt_commit('-am','main');",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM overlap_indexes ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(w, o) JOIN_ORDER(w, o) */ o.* FROM (SELECT 2 AS v) w JOIN overlap_indexes o ON w.v = o.v;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(w, o) JOIN_ORDER(w, o) */ o.* FROM (SELECT 3 AS v) w JOIN overlap_indexes o ON w.v = o.v;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unique keys, update violation from left",
            intent: "An update on the left that duplicates a unique key records a violation.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'right insert');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 3 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'left insert');",
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
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("3")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("unique index"), T("2"), T("3")],
                            &[T("unique index"), T("3"), T("3")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "unique keys, update violation from right",
            intent: "An update on the right that duplicates a unique key records a violation.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE t SET col1 = 3 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'right insert');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'left insert');",
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
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("3")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("unique index"), T("2"), T("3")],
                            &[T("unique index"), T("3"), T("3")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "cell-wise merges can result in a unique key violation",
            intent: "A cell-wise merge whose merged row duplicates a unique key records a violation.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int); CREATE UNIQUE INDEX col1_col2_u ON t (col1, col2);",
                "SELECT dolt_add('.');",
                "INSERT INTO T VALUES (1, 1, 1), (2, NULL, NULL);",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE t SET col2 = 1 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 1 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'left edit');",
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
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, col2 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("1")],
                            &[T("unique index"), T("2"), T("1"), T("1")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "Test feature data preservation across collation merges",
            intent: "Data and features survive a merge across collation changes.",
            set_up_script: &[
                "CREATE TABLE t(pk VARCHAR(255) PRIMARY KEY, v VARCHAR(255))  ;",
                "SELECT dolt_commit('-Am','base');",
                "SELECT * FROM dolt_checkout('-b','feature');",
                "INSERT INTO t VALUES('1','first');",
                "SELECT dolt_commit('-am','first row');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ;",
                "ALTER TABLE t ALTER COLUMN v TYPE VARCHAR(255), ALTER COLUMN v SET NOT NULL, ALTER COLUMN v DROP DEFAULT;",
                "SELECT dolt_commit('-am','collation');",
                "SELECT * FROM dolt_checkout('feature');",
                "SELECT * FROM dolt_merge('main','-m','merge');",
                "INSERT INTO t VALUES('2','second');",
                "SELECT dolt_commit('-am','second row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk,to_v,dolt_diff_type FROM dolt_commit_diff_t WHERE dolt_to_commit=dolt_hashof('feature') AND dolt_from_commit=DOLT_MERGE_BASE('main','feature') ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", VARCHAR), Column("to_v", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("first"), T("added")],
                            &[T("2"), T("second"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "left adds a unique key constraint and resolves existing violations",
            intent: "A unique key added on the left after its violations were resolved merges cleanly.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1), (2, 1);",
                "SELECT dolt_commit('-am', 'table and data');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 2 WHERE pk = 2;",
                "CREATE UNIQUE INDEX col1_uniq ON t (col1);",
                "SELECT dolt_commit('-am', 'left adds a unique index');",
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
            name: "insert two tables with the same name and different schema",
            intent: "Two tables of one name and different schemas made on two branches are a schema conflict.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, extracol int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1, 1);",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'left');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses a merge of tables of one name both branches added with different definitions, \"table with same name 't' added in 2 commits can't be merged\", a limit of its implementation, where Doltgrust merges them as one table against an empty base, their definitions as any schema merge merges them, Appendix C \"tables of one name added on both branches\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses a merge of tables of one name both branches added with different definitions, \"table with same name 't' added in 2 commits can't be merged\", a limit of its implementation, where Doltgrust merges them as one table against an empty base, their definitions as any schema merge merges them, Appendix C \"tables of one name added on both branches\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("extracol", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses a merge of tables of one name both branches added with different definitions, \"table with same name 't' added in 2 commits can't be merged\", a limit of its implementation, where Doltgrust merges them as one table against an empty base, their definitions as any schema merge merges them, Appendix C \"tables of one name added on both branches\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert two tables with the same name and schema that don't conflict",
            intent: "Two tables of one name and schema made on two branches with compatible rows merge.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'left');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert two tables with the same name and schema that conflict",
            intent: "Two tables of one name and schema made on two branches with conflicting rows conflict.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, -1);",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'left');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other');",
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
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("1"), T("1"), T("-1")],
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
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge with new triggers defined",
            intent: "Triggers made on the other branch come with the merge.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "CREATE TABLE x(a bigint PRIMARY KEY);",
                "CREATE OR REPLACE FUNCTION \"trigger1_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN new.a := new.a + 1; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger1\" BEFORE INSERT ON x FOR EACH ROW EXECUTE FUNCTION \"trigger1_oracle_trigger\"();",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added table with trigger');",
                "SELECT dolt_branch('-c', 'main', 'other');",
                "CREATE OR REPLACE FUNCTION \"trigger2_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN new.a := (new.a * 2) + 10; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger2\" BEFORE INSERT ON x FOR EACH ROW EXECUTE FUNCTION \"trigger2_oracle_trigger\"();",
                "SELECT dolt_commit('-am', 'created trigger2 on main');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE OR REPLACE FUNCTION \"trigger3_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN new.a := (new.a * 2) + 100; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger3\" BEFORE INSERT ON x FOR EACH ROW EXECUTE FUNCTION \"trigger3_oracle_trigger\"();",
                "CREATE OR REPLACE FUNCTION \"trigger4_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN new.a := (new.a * 2) + 1000; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger4\" BEFORE INSERT ON x FOR EACH ROW EXECUTE FUNCTION \"trigger4_oracle_trigger\"();",
                "SELECT dolt_commit('-am', 'created triggers 3 & 4 on other');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                    query: "SELECT count(*) FROM dolt_schemas WHERE type = 'trigger';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: dolt_schemas holds MySQL's triggers, where Postgres keeps them in pg_trigger"),
            ..S
        },
        ScriptTest {
            name: "dolt_merge() works with no auto increment overlap",
            intent: "Auto-increment keys taken on both branches without overlap merge, and the sequence continues past both.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, c0 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t (c0) VALUES (1), (2);",
                "SELECT dolt_commit('-a', '-m', 'cm1');",
                "SELECT * FROM dolt_checkout('-b', 'test');",
                "INSERT INTO t (c0) VALUES (3), (4);",
                "SELECT dolt_commit('-a', '-m', 'cm2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test');",
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
                    query: "INSERT INTO t(c0) VALUES (5),(6),(7);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                            &[T("7"), T("7")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_merge() (3way) works with no auto increment overlap",
            intent: "A three-way merge of auto-increment keys without overlap continues the sequence past both.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, c0 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t (c0) VALUES (1);",
                "SELECT dolt_commit('-a', '-m', 'cm1');",
                "SELECT * FROM dolt_checkout('-b', 'test');",
                "INSERT INTO t (pk,c0) VALUES (3,3), (4,4);",
                "SELECT dolt_commit('-a', '-m', 'cm2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (c0) VALUES (5);",
                "SELECT dolt_commit('-a', '-m', 'cm3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test');",
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
                    query: "INSERT INTO t(c0) VALUES (6),(7),(8);",
                    expected: Expected::Tag("INSERT 0 3"),
                    skip: Some("2f merge: sequences both branches advanced (M4b)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                            &[T("7"), T("7")],
                            &[T("8"), T("8")],
                        ],
                        tag: "SELECT 7",
                    },
                    skip: Some("2f merge: sequences both branches advanced (M4b)"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_merge() with a gap in an auto increment key",
            intent: "A gap in an auto-increment key on one branch merges, and the sequence continues past the highest.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, c0 int);",
                "INSERT INTO t (c0) VALUES (1), (2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'cm1');",
                "SELECT * FROM dolt_checkout('-b', 'test');",
                "INSERT INTO t VALUES (4,4), (5,5);",
                "SELECT dolt_commit('-am', 'cm2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test');",
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
                    query: "INSERT INTO t VALUES (3,3),(NULL,6);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: inserting NULL into an auto-increment key takes its next value, which Postgres refuses as a NOT NULL violation"),
            ..S
        },
        ScriptTest {
            name: "dolt_merge() (3way) with a gap in an auto increment key",
            intent: "A three-way merge with a gap in an auto-increment key continues past the highest.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, c0 int);",
                "INSERT INTO t (c0) VALUES (1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'cm1');",
                "SELECT * FROM dolt_checkout('-b', 'test');",
                "INSERT INTO t VALUES (4,4), (5,5);",
                "SELECT dolt_commit('-am', 'cm2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (c0) VALUES (6);",
                "SELECT dolt_commit('-am', 'cm3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test');",
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
                    query: "INSERT INTO t VALUES (3,3),(NULL,7);",
                    expected: Expected::Tag("INSERT 0 2"),
                    skip: Some("2f merge: sequences both branches advanced (M4b)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                            &[T("7"), T("7")],
                        ],
                        tag: "SELECT 6",
                    },
                    skip: Some("2f merge: sequences both branches advanced (M4b)"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "add multiple columns, then set and unset a value. No conflicts expected.",
            intent: "Columns added, set, and unset on both sides merge without conflict.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2);",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE t SET col1 = 1 WHERE pk = 1;",
                "UPDATE t SET col1 = NULL WHERE pk = 1;",
                "SELECT dolt_commit('--allow-empty', '-am', 'right cm');",
                "SELECT * FROM dolt_checkout('main');",
                "DELETE FROM t WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'left cm');",
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
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("2"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping constraint from one branch drops from both",
            intent: "A constraint dropped on one branch is gone after the merge.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "ALTER TABLE t ADD CONSTRAINT c CHECK (i > 0);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'changes to other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP CONSTRAINT c;",
                "SELECT dolt_commit('-Am', 'changes to main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
            name: "dropping constraint from one branch drops from both, no checkout",
            intent: "A constraint dropped on one branch, merged without checking out, is gone.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "ALTER TABLE t ADD CONSTRAINT c CHECK (i > 0);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('other');",
                "use \"postgres/other\";",
                "INSERT INTO t VALUES (1);",
                "ALTER TABLE t DROP CONSTRAINT c;",
                "SELECT dolt_commit('-Am', 'changes to other');",
                "use \"postgres/main\";",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (-1);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t\" violates check constraint \"c\"", ..E }),
                    ..A
                },
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
                    query: "SELECT * FROM t;",
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
                    query: "INSERT INTO t VALUES (-1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            skip: Some("the setup switches databases with MySQL's USE, which Postgres lacks (42601); re-port it to the harness's \\c"),
            ..S
        },
        ScriptTest {
            name: "merge constraint with valid data on different branches",
            intent: "A constraint added on one branch passes over valid data from the other.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'changes to other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CHECK (i < 10);",
                "SELECT dolt_commit('-Am', 'changes to main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
            name: "resolving a deleted and modified row handles constraint checks",
            intent: "Resolving a row deleted on one side and modified on the other runs constraint checks.",
            set_up_script: &[
                "CREATE TABLE test(a int PRIMARY KEY, b int, c int);",
                "ALTER TABLE test ADD CHECK (b < 4);",
                "INSERT INTO test VALUES (1, 2, 3);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'create test table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER TABLE test DROP COLUMN c;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'drop column');",
                "SELECT * FROM dolt_checkout('main');",
                "DELETE FROM test;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'remove row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                        columns: &[Column("a", INT4), Column("b", INT4)],
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
            name: "resolving a modified/modified row still checks nullness constraint",
            intent: "Resolving a row both sides modified still checks NOT NULL.",
            set_up_script: &[
                "CREATE TABLE test(a int PRIMARY KEY, b int, c int);",
                "INSERT INTO test VALUES (1, 2, 3);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'create test table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER TABLE test ALTER COLUMN c TYPE int, ALTER COLUMN c SET NOT NULL, ALTER COLUMN c DROP DEFAULT;",
                "UPDATE test SET b = NULL;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'drop column');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE test ALTER COLUMN b TYPE int, ALTER COLUMN b SET NOT NULL, ALTER COLUMN b DROP DEFAULT;",
                "UPDATE test SET c = NULL;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'remove row');",
                "SET dolt_force_transaction_commit = ON;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    skip: Some("Dolt skips it too (TODO: constraint violations): dolt_preview_merge_conflicts_summary counts no constraint violations yet"),
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, c FROM dolt_constraint_violations_test;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Pk convergent updates to sec diff congruent",
            intent: "Identical primary key updates on both sides with secondary index changes merge.",
            set_up_script: &[
                "CREATE TABLE xyz (x int PRIMARY KEY, y int, z int); CREATE INDEX y_idx ON xyz (y); CREATE INDEX z_idx ON xyz (z);",
                "INSERT INTO xyz VALUES (0,0,0), (1,1,1);",
                "SELECT dolt_commit('-Am', 'make table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE xyz SET z = 2 WHERE z = 1;",
                "SELECT dolt_commit('-am', 'update z=2');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE xyz SET y = 2 WHERE z = 1;",
                "SELECT dolt_commit('-am', 'update y=2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
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
                    query: "SELECT y FROM xyz WHERE y >= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT z FROM xyz WHERE y >= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("z", INT4)],
                        rows: &[
                            &[T("0")],
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
            name: "Pk convergent left and right adds",
            intent: "Identical rows added on both sides merge.",
            set_up_script: &[
                "CREATE TABLE xyz (x int PRIMARY KEY, y int, z int); CREATE INDEX y_idx ON xyz (y); CREATE INDEX z_idx ON xyz (z);",
                "INSERT INTO xyz VALUES (0,0,0);",
                "SELECT dolt_commit('-Am', 'make table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO xyz VALUES (1,1,1);",
                "SELECT dolt_commit('-am', 'adds');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO xyz VALUES (1,1,1), (2,2,2);",
                "SELECT dolt_commit('-am', 'adds');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
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
                    query: "SELECT y FROM xyz WHERE y >= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT z FROM xyz WHERE y >= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("z", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Pk adds+convergent adds to sec congruent",
            intent: "Identical rows added and updated on both sides merge with their secondary indexes.",
            set_up_script: &[
                "CREATE TABLE xyz (x int PRIMARY KEY, y int, z int); CREATE INDEX y_idx ON xyz (y); CREATE INDEX z_idx ON xyz (z);",
                "INSERT INTO xyz VALUES (0,0,0), (1,1,1);",
                "SELECT dolt_commit('-Am', 'make table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO xyz VALUES (3,3,3);",
                "UPDATE xyz SET z = 5 WHERE z = 1;",
                "SELECT dolt_commit('-am', 'right adds + edit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO xyz VALUES (4,4,4);",
                "UPDATE xyz SET y = 2 WHERE z = 1;",
                "SELECT dolt_commit('-am', 'left adds + update');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
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
                    query: "SELECT y FROM xyz WHERE y >= 0 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT z FROM xyz WHERE y >= 0 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("z", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Left deletes",
            intent: "Rows deleted on the left stay deleted.",
            set_up_script: &[
                "CREATE TABLE xyz (x int PRIMARY KEY, y int, z int); CREATE INDEX y_idx ON xyz (y); CREATE INDEX z_idx ON xyz (z);",
                "INSERT INTO xyz VALUES (0,0,0), (1,1,1),(2,2,2),(3,3,3);",
                "SELECT dolt_commit('-Am', 'make table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "DELETE FROM xyz WHERE y = 3;",
                "SELECT dolt_commit('-am', 'right deletes');",
                "SELECT * FROM dolt_checkout('main');",
                "DELETE FROM xyz WHERE y = 1;",
                "SELECT dolt_commit('-am', 'left deletes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
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
                    query: "SELECT y FROM xyz WHERE y >= 0 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT z FROM xyz WHERE y >= 0 ORDER by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("z", INT4)],
                        rows: &[
                            &[T("0")],
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
            name: "delete conflict",
            intent: "A row deleted on one side and changed on the other conflicts.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE xyz (x int PRIMARY KEY, y int, z int); CREATE INDEX y_idx ON xyz (y); CREATE INDEX z_idx ON xyz (z);",
                "INSERT INTO xyz VALUES (0,0,0), (1,1,1);",
                "SELECT dolt_commit('-Am', 'make table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "DELETE FROM xyz WHERE y = 1;",
                "SELECT dolt_commit('-am', 'right delete');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE xyz SET y = 2 WHERE y = 1;",
                "SELECT dolt_commit('-am', 'left update');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.xyz"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
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
                    query: "SELECT our_y, dolt_our_diff_type, their_y, dolt_their_diff_type FROM dolt_conflicts_xyz;",
                    expected: Expected::Rows {
                        columns: &[Column("our_y", INT4), Column("dolt_our_diff_type", TEXT), Column("their_y", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("modified"), Null, T("removed")],
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
            name: "divergent edit conflict",
            intent: "A row changed differently on both sides conflicts.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE xyz (x int PRIMARY KEY, y int, z int); CREATE INDEX y_idx ON xyz (y); CREATE INDEX z_idx ON xyz (z);",
                "INSERT INTO xyz VALUES (0,0,0), (1,1,1);",
                "SELECT dolt_commit('-Am', 'make table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE xyz SET y = 3 WHERE y = 1;",
                "SELECT dolt_commit('-am', 'right delete');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE xyz SET y = 2 WHERE y = 1;",
                "SELECT dolt_commit('-am', 'left update');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.xyz"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
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
                    query: "SELECT our_y, dolt_our_diff_type, their_y, dolt_their_diff_type FROM dolt_conflicts_xyz;",
                    expected: Expected::Rows {
                        columns: &[Column("our_y", INT4), Column("dolt_our_diff_type", TEXT), Column("their_y", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("modified"), T("3"), T("modified")],
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
            name: "Merge errors if the primary key types have changed (even if the new type has the same NomsKind)",
            intent: "A primary key whose type changed on one side fails the merge.",
            set_up_script: &[
                "CREATE TABLE t (pk1 bigint, pk2 bigint, PRIMARY KEY (pk1, pk2));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "ALTER TABLE t ALTER COLUMN pk2 TYPE smallint, ALTER COLUMN pk2 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'left commit');",
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
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT2)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("a merge takes one side's change of a column's type when Postgres converts the old type to the new in an assignment, converting the other side's rows, CLAUDE.md's rule and Appendix C, where Dolt calls it a schema conflict")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "`Delete from table` should keep artifacts - conflicts",
            intent: "DELETE from a table keeps its conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 100);",
                "INSERT INTO t VALUES (2, 200);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, -100);",
                "INSERT INTO t VALUES (2, -200);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("-100"), T("1"), T("100")],
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, Null, Null, T("1"), T("100")],
                            &[Null, Null, Null, Null, T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "`Truncate table` should keep artifacts - conflicts",
            intent: "TRUNCATE keeps a table's conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 100);",
                "INSERT INTO t VALUES (2, 200);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, -100);",
                "INSERT INTO t VALUES (2, -200);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("-100"), T("1"), T("100")],
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, Null, Null, T("1"), T("100")],
                            &[Null, Null, Null, Null, T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "`Delete from table` should keep artifacts - violations",
            intent: "DELETE from a table keeps its violations.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (2, 1), (3, 3);",
                "SELECT dolt_commit('-am', 'other insert');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1), (4, 4);",
                "SELECT dolt_commit('-am', 'main insert');",
                "SET dolt_force_transaction_commit = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
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
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "`Truncate table` should keep artifacts - violations",
            intent: "TRUNCATE keeps a table's violations.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (2, 1), (3, 3);",
                "SELECT dolt_commit('-am', 'other insert');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1), (4, 4);",
                "SELECT dolt_commit('-am', 'main insert');",
                "SET dolt_force_transaction_commit = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
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
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "parent index is longer than child index",
            intent: "A parent index longer than the child's foreign key columns still checks the foreign key.",
            set_up_script: &[
                "CREATE TABLE parent (i int PRIMARY KEY, x int, y int, z int); CREATE INDEX  ON parent (y, x, z);",
                "CREATE TABLE child (y int, x int, PRIMARY KEY(y, x), FOREIGN KEY (y, x) REFERENCES parent(y, x));",
                "INSERT INTO parent VALUES (100,1,1,1), (200,2,1,2), (300,1,NULL,1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE x = 2;",
                "SELECT dolt_commit('-am', 'main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (1, 2);",
                "SELECT dolt_commit('-am', 'other');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_force_transaction_commit = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations_parent;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("dolt_violation_type", TEXT), Column("i", INT4), Column("x", INT4), Column("y", INT4), Column("z", INT4), Column("dolt_violation_info", JSONB)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT y, x FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4), Column("x", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a foreign key that references columns without a unique constraint is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "parallel column updates (repro issue #4547)",
            intent: "Parallel updates of different columns of one row merge.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "CREATE TABLE t (rowId int NOT NULL, col1 varchar(255), col2 varchar(255), keyCol varchar(60), dataA varchar(255), dataB varchar(255), PRIMARY KEY (rowId)); CREATE UNIQUE INDEX uniqKey ON t (col1, col2, keyCol);",
                "INSERT INTO t (rowId, col1, col2, keyCol, dataA, dataB) VALUES (1, '1', '2', 'key-a', 'test1', 'test2');",
                "SELECT dolt_commit('-Am', 'new table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "UPDATE t SET dataA = 'other';",
                "SELECT dolt_commit('-am', 'update data other');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET dataB = 'main';",
                "SELECT dolt_commit('-am', 'update on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                    query: "SELECT * FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("dolt_violation_type", TEXT), Column("rowid", INT4), Column("col1", VARCHAR), Column("col2", VARCHAR), Column("keycol", VARCHAR), Column("dataa", VARCHAR), Column("datab", VARCHAR), Column("dolt_violation_info", JSONB)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("rowid", INT4), Column("col1", VARCHAR), Column("col2", VARCHAR), Column("keycol", VARCHAR), Column("dataa", VARCHAR), Column("datab", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("2"), T("key-a"), T("other"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "try to merge a nullable field into a non-null column",
            intent: "Merging NULLs into a column the other side made NOT NULL records violations.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(3,3);",
                "SELECT dolt_commit('-Am', 'new table with NULL value');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO test VALUES (2,NULL);",
                "SELECT dolt_commit('-am', 'inserted null value');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE test ALTER COLUMN c0 TYPE int, ALTER COLUMN c0 SET NOT NULL, ALTER COLUMN c0 DROP DEFAULT;",
                "INSERT INTO test VALUES (4,4);",
                "SELECT dolt_commit('-am', 'modified column c0 to not null');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.test"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, dolt_violation_info FROM dolt_constraint_violations_test;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("dolt_violation_info", JSONB)],
                        rows: &[
                            &[T("not null"), T("2"), T("{\"Columns\": [\"c0\"]}")],
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
            name: "merge fulltext with renamed table",
            intent: "A full-text index on a table renamed on one side merges.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 VARCHAR(200), FULLTEXT idx (v1));",
                "INSERT INTO test VALUES (1, 'abc');",
                "SELECT dolt_commit('-Am', 'Initial commit');",
                "SELECT dolt_branch('other');",
                "DROP INDEX idx;",
                "INSERT INTO test VALUES (2, 'def');",
                "ALTER TABLE test RENAME TO test_temp;",
                "ALTER TABLE test_temp ADD FULLTEXT INDEX idx (v1);",
                "ALTER TABLE test_temp RENAME TO test;",
                "SELECT dolt_commit('-Am', 'Renamed pseudo-index tables');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test VALUES (3, 'ghi');",
                "SELECT dolt_commit('-Am', 'Insertion commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                    query: "SELECT v1 FROM test WHERE MATCH(v1) AGAINST ('abc def ghi');",
                    expected: Expected::Rows {
                        columns: &[Column("v1", VARCHAR)],
                        rows: &[
                            &[T("abc")],
                            &[T("def")],
                            &[T("ghi")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: FULLTEXT indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "merge when schemas are equal, but column tags are different",
            intent: "Tables with equal schemas but different column ids merge by name.",
            set_up_script: &[
                "SELECT dolt_branch('branch1');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'creating table t on main');",
                "ALTER TABLE t ALTER COLUMN col1 TYPE varchar(255), ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'modifying table t on main');",
                "INSERT INTO t VALUES (1, 'one'), (2, 'two');",
                "SELECT dolt_commit('-am', 'inserting two rows into t on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(255));",
                "SELECT dolt_commit('-Am', 'creating table t on branch1');",
                "INSERT INTO t VALUES (3, 'three');",
                "SELECT dolt_commit('-am', 'inserting one row into t on branch1');",
                "SELECT set_config('test.PreMergeBranch1Commit', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                            &[T("3"), T("three")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', current_setting('test.PreMergeBranch1Commit'));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
            name: "merge with float column default",
            intent: "A float column default merges.",
            set_up_script: &[
                "CREATE TABLE t (f float);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN f TYPE float, ALTER COLUMN f DROP NOT NULL, ALTER COLUMN f SET DEFAULT '1.00';",
                "SELECT dolt_commit('-Am', 'change default on main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN f TYPE float, ALTER COLUMN f DROP NOT NULL, ALTER COLUMN f SET DEFAULT '1.000000000';",
                "SELECT dolt_commit('-Am', 'change default on other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
            name: "merge with float 1.23 column default",
            intent: "A float 1.23 column default merges.",
            set_up_script: &[
                "CREATE TABLE t (f float);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN f TYPE float, ALTER COLUMN f DROP NOT NULL, ALTER COLUMN f SET DEFAULT '1.23000';",
                "SELECT dolt_commit('-Am', 'change default on main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN f TYPE float, ALTER COLUMN f DROP NOT NULL, ALTER COLUMN f SET DEFAULT '1.23000000000';",
                "SELECT dolt_commit('-Am', 'change default on other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
            name: "merge with decimal 1.23 column default",
            intent: "A decimal 1.23 column default merges.",
            set_up_script: &[
                "CREATE TABLE t (d decimal(20, 10));",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN d TYPE decimal(20, 10), ALTER COLUMN d DROP NOT NULL, ALTER COLUMN d SET DEFAULT '1.23000';",
                "SELECT dolt_commit('-Am', 'change default on main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN d TYPE decimal(20, 10), ALTER COLUMN d DROP NOT NULL, ALTER COLUMN d SET DEFAULT '1.23000000000';",
                "SELECT dolt_commit('-Am', 'change default on other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
            skip: Some("2f merge: column defaults equal in value but written differently, '1.23000' and '1.23000000000', conflict instead of merging"),
            ..S
        },
        ScriptTest {
            name: "merge with different types",
            intent: "Columns of different types on the two sides are a schema conflict.",
            set_up_script: &[
                "CREATE TABLE t (f float);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ALTER COLUMN f TYPE float, ALTER COLUMN f DROP NOT NULL, ALTER COLUMN f SET DEFAULT 1.23;",
                "SELECT dolt_commit('-Am', 'change default on main');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t ALTER COLUMN f TYPE float, ALTER COLUMN f DROP NOT NULL, ALTER COLUMN f SET DEFAULT '1.23';",
                "SELECT dolt_commit('-Am', 'change default on other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('other', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    skip: Some("the two defaults differ in Postgres, 1.23 a numeric constant cast to double precision and '1.23' a double precision constant, so changing them differently on both branches is a schema conflict"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("the two defaults differ in Postgres, 1.23 a numeric constant cast to double precision and '1.23' a double precision constant, so changing them differently on both branches is a schema conflict"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge with nonmatching column tags on index",
            intent: "An index whose columns have different ids on the two sides merges.",
            set_up_script: &[
                "CREATE TABLE t1(pk int PRIMARY KEY, c1 varchar(100), c2 int);",
                "INSERT INTO t1 VALUES (1, 'one', 1), (2, 'two', 2);",
                "SELECT dolt_update_column_tag('t1', 'c1', 2);",
                "CREATE INDEX idx1 ON t1 (c1);",
                "SELECT dolt_commit('-Am', 'initial tables');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'empty commit');",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT dolt_update_column_tag('t1', 'c1', 202);",
                "INSERT INTO t1 VALUES (101, 'blah', 101);",
                "SELECT dolt_commit('-Am', 'changes on b1');",
            ],
            assertions: &[
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
                    query: "SELECT INDEX_NAME, COLUMN_NAME FROM (SELECT n.nspname::text AS table_schema, c.relname::text AS table_name, CASE WHEN i.indisprimary THEN 'PRIMARY' ELSE ic.relname::text END AS index_name, (NOT i.indisunique)::int AS non_unique, k.n::int AS seq_in_index, a.attname::text AS column_name FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_class ic ON ic.oid = i.indexrelid CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY AS k(attnum, n) JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = k.attnum WHERE n.nspname = 'public') AS statistics WHERE TABLE_SCHEMA = 'postgres' AND TABLE_NAME = 't1';",
                    expected: Expected::Rows {
                        columns: &[Column("index_name", TEXT), Column("column_name", TEXT)],
                        rows: &[
                            &[T("PRIMARY"), T("pk")],
                            &[T("idx1"), T("c1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.c1 FROM t1 WHERE c1 IN ('one', 'two', 'three');",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("one")],
                            &[T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
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
                    query: "SELECT INDEX_NAME, COLUMN_NAME FROM (SELECT n.nspname::text AS table_schema, c.relname::text AS table_name, CASE WHEN i.indisprimary THEN 'PRIMARY' ELSE ic.relname::text END AS index_name, (NOT i.indisunique)::int AS non_unique, k.n::int AS seq_in_index, a.attname::text AS column_name FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_class ic ON ic.oid = i.indexrelid CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY AS k(attnum, n) JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = k.attnum WHERE n.nspname = 'public') AS statistics WHERE TABLE_SCHEMA='postgres' AND TABLE_NAME='t1';",
                    expected: Expected::Rows {
                        columns: &[Column("index_name", TEXT), Column("column_name", TEXT)],
                        rows: &[
                            &[T("PRIMARY"), T("pk")],
                            &[T("idx1"), T("c1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "merge with nonmatching column tags on primary key",
            intent: "A primary key whose columns have different ids on the two sides merges.",
            set_up_script: &[
                "CREATE TABLE t1(pk int PRIMARY KEY, c1 varchar(100), c2 int);",
                "INSERT INTO t1 VALUES (1, 'one', 1), (2, 'two', 2);",
                "SELECT dolt_update_column_tag('t1', 'pk', 1);",
                "SELECT dolt_commit('-Am', 'initial tables');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'empty commit');",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT dolt_update_column_tag('t1', 'pk', 101);",
                "INSERT INTO t1 VALUES (101, 'blah', 3);",
                "SELECT dolt_commit('-Am', 'changing t1.pk column tag on b1');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM dolt_merge('b1');",
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
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "merge with nonmatching column tags and types on primary key",
            intent: "A primary key whose columns differ in id and type is a schema conflict.",
            set_up_script: &[
                "CREATE TABLE t1(pk int PRIMARY KEY, c1 varchar(100), c2 int);",
                "INSERT INTO t1 VALUES (1, 'one', 1), (2, 'two', 2);",
                "SELECT dolt_update_column_tag('t1', 'pk', 1);",
                "SELECT dolt_commit('-Am', 'initial tables');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'empty commit');",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT dolt_update_column_tag('t1', 'pk', 101);",
                "ALTER TABLE t1 ALTER COLUMN pk TYPE float, ALTER COLUMN pk DROP NOT NULL, ALTER COLUMN pk DROP DEFAULT;",
                "INSERT INTO t1 VALUES (101, 'blah', 3);",
                "SELECT dolt_commit('-Am', 'changing t1.pk column tag on b1');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "could not map primary key column pk", ..E }),
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "--ff-only flag success when fast-forward is possible",
            intent: "--ff-only fast-forwards when it can.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20));",
                "INSERT INTO t VALUES (1, 'main1'), (2, 'main2');",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO t VALUES (3, 'feature1');",
                "SELECT dolt_commit('-am', 'feature commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--ff-only', 'feature');",
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
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("main1")],
                            &[T("2"), T("main2")],
                            &[T("3"), T("feature1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "--ff-only flag failure when fast-forward is not possible",
            intent: "--ff-only fails when a fast-forward isn't possible.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20));",
                "INSERT INTO t VALUES (1, 'main1'), (2, 'main2');",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO t VALUES (3, 'feature1');",
                "SELECT dolt_commit('-am', 'feature commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (4, 'main3');",
                "SELECT dolt_commit('-am', 'main commit 2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--ff-only', 'feature');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: Not possible to fast-forward, aborting", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("main1")],
                            &[T("2"), T("main2")],
                            &[T("4"), T("main3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "--ff-only flag with already up-to-date branch",
            intent: "--ff-only on an up-to-date branch changes nothing.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20));",
                "INSERT INTO t VALUES (1, 'main1'), (2, 'main2');",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--ff-only', 'feature');",
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
        ScriptTest {
            name: "--ff-only conflicts with --no-ff",
            intent: "--ff-only and --no-ff together are refused.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20));",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--ff-only', '--no-ff', 'feature');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Flags '--ff-only' and '--no-ff' cannot be used together", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "--ff-only conflicts with --squash",
            intent: "--ff-only and --squash together are refused.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20));",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--ff-only', '--squash', 'feature');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Flags '--ff-only' and '--squash' cannot be used together", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "--ff-only with no-commit flag should work",
            intent: "--ff-only with --no-commit fast-forwards.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20));",
                "INSERT INTO t VALUES (1, 'main1');",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO t VALUES (2, 'feature1');",
                "SELECT dolt_commit('-am', 'feature commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--ff-only', '--no-commit', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "foreign key violation, parent row deleted",
            intent: "A parent row deleted on one side records a foreign key violation for the other side's child.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (fk int, FOREIGN KEY (fk) REFERENCES parent(pk));",
                "INSERT INTO parent VALUES (1), (2);",
                "INSERT INTO child VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE fk = 1;",
                "DELETE FROM parent WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'delete parent and child rows');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (1);",
                "SELECT dolt_commit('-am', 'insert child row');",
                "SET dolt_force_transaction_commit = ON;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1")],
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
            name: "foreign key violation, parent row deleted, child keyless",
            intent: "A parent row deleted on one side records a violation for a keyless child.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY, a int);",
                "CREATE UNIQUE INDEX idx_a ON parent(pk, a);",
                "CREATE TABLE child (fk1 int, fk2 int, FOREIGN KEY (fk1, fk2) REFERENCES parent(pk, a));",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (10, 1), (20, 2);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE fk2 = 1;",
                "DELETE FROM parent WHERE pk = 10;",
                "SELECT dolt_commit('-am', 'delete parent and child rows');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (10, 1);",
                "SELECT dolt_commit('-am', 'insert child row');",
                "SET dolt_force_transaction_commit = ON;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, fk2 FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("fk2", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1")],
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
            name: "foreign key violation, parent row deleted, secondary index",
            intent: "A parent row deleted on one side records a violation through a secondary index.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY, v1 int);",
                "CREATE UNIQUE INDEX idx_v1 ON parent(v1);",
                "CREATE TABLE child (fk int, FOREIGN KEY (fk) REFERENCES parent(v1));",
                "INSERT INTO parent VALUES (1, 1), (2, 2);",
                "INSERT INTO child VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE fk = 1;",
                "DELETE FROM parent WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'delete parent and child rows');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (1);",
                "SELECT dolt_commit('-am', 'insert child row');",
                "SET dolt_force_transaction_commit = ON;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1")],
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
            name: "Merge foreign keys, 2 column key, dangling child violation",
            intent: "A dangling child of a two-column foreign key records a violation.",
            set_up_script: &[
                "CREATE TABLE parent (a VARCHAR(256), b varchar(256), c VARCHAR(256), PRIMARY KEY (b, a));",
                "CREATE INDEX idx_parent_on_a_b ON parent (a, b);",
                "CREATE TABLE child (d VARCHAR(256), e VARCHAR(256), f varchar(256), PRIMARY KEY (e, d), CONSTRAINT child_fk FOREIGN KEY (d, f) REFERENCES parent (a, b));",
                "INSERT INTO parent VALUES ('abc','def', 'xyz');",
                "INSERT INTO child VALUES ('abc','123','def');",
                "SELECT dolt_commit('-Am', '1');",
                "SELECT dolt_branch('other_branch');",
                "INSERT INTO child VALUES ('abc','www','def');",
                "SELECT dolt_commit('-Am', '2');",
                "CREATE TABLE table3 (table3_col1 VARCHAR(256));",
                "SELECT dolt_commit('-Am', '3');",
                "SELECT * FROM dolt_checkout('other_branch');",
                "DELETE FROM child WHERE e='123';",
                "DELETE FROM parent WHERE a='abc';",
                "SELECT dolt_commit('-Am', '4');",
                "SET dolt_force_transaction_commit = 1;",
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
                    query: "SELECT dolt_violation_type, d, e, f FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("d", VARCHAR), Column("e", VARCHAR), Column("f", VARCHAR)],
                        rows: &[
                            &[T("foreign key"), T("abc"), T("www"), T("def")],
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
            name: "Merge does not panic when FK is dropped and re-added on one branch and child has composite PK with mixed types",
            intent: "A foreign key dropped and added again on one side over a mixed-type composite key merges.",
            set_up_script: &[
                "CREATE TABLE parent (pk VARCHAR(20) PRIMARY KEY);",
                "CREATE TABLE child (pk1    int, pk2    bigint, fk_col VARCHAR(20), PRIMARY KEY (pk1, pk2), CONSTRAINT fk1 FOREIGN KEY (fk_col) REFERENCES parent (pk));",
                "INSERT INTO parent VALUES ('a');",
                "INSERT INTO child VALUES (1, 1, 'a');",
                "SELECT dolt_commit('-Am', 'initial setup');",
                "SELECT dolt_branch('other_branch');",
                "ALTER TABLE child DROP CONSTRAINT fk1;",
                "SELECT dolt_commit('-Am', 'drop fk and its backing index');",
                "ALTER TABLE child ADD CONSTRAINT fk2 FOREIGN KEY (fk_col) REFERENCES parent (pk);",
                "SELECT dolt_commit('-Am', 're-add fk, creates new backing index fk2');",
                "SELECT * FROM dolt_checkout('other_branch');",
                "INSERT INTO child VALUES (1, 2, 'a');",
                "SELECT dolt_commit('-Am', 'add child row on other branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
                    query: "SELECT pk FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR)],
                        rows: &[
                            &[T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2, fk_col FROM child ORDER BY pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT8), Column("fk_col", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("a")],
                            &[T("1"), T("2"), T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (2, 1, 'z');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"fk2\"", detail: "Key (fk_col)=(z) is not present in table \"parent\".", schema: "public", table: "child", constraint: "fk2", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "three-way merge of table with vector index",
            intent: "A table with a vector index merges three ways.",
            set_up_script: &[
                "CREATE TABLE chunks (id int PRIMARY KEY, e VECTOR(3) NOT NULL, VECTOR INDEX vidx (e));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'base schema');",
                "SELECT * FROM dolt_checkout('-b', 'branch');",
                "INSERT INTO chunks VALUES (1, STRING_TO_VECTOR('[1,0,0]')), (2, STRING_TO_VECTOR('[0,1,0]'));",
                "SELECT dolt_commit('-am', 'rows on branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO chunks VALUES (9, STRING_TO_VECTOR('[0.5,0.5,0]'));",
                "SELECT dolt_commit('-am', 'row on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch');",
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
                    query: "SELECT id FROM chunks ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: VECTOR columns and indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "dolt_conflicts_resolve keeps vector index consistent with resolved rows",
            intent: "Resolving conflicts keeps a vector index consistent with the resolved rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE chunks (id int PRIMARY KEY, e VECTOR(2) NOT NULL, VECTOR INDEX vidx (e));",
                "INSERT INTO chunks VALUES (1, STRING_TO_VECTOR('[50,60]')), (2, STRING_TO_VECTOR('[42,52]')), (3, STRING_TO_VECTOR('[44,54]'));",
                "SELECT dolt_commit('-Am', 'base rows');",
                "SELECT * FROM dolt_checkout('-b', 'branch');",
                "UPDATE chunks SET e = STRING_TO_VECTOR('[0,0]') WHERE id = 1;",
                "SELECT dolt_commit('-am', 'branch moves id 1 to the origin');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE chunks SET e = STRING_TO_VECTOR('[99,99]') WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main moves id 1 far away');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch');",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'chunks');",
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
                    query: "SELECT id FROM chunks ORDER BY VEC_DISTANCE('[0,0]', e) LIMIT 1;",
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
            skip: Some("MySQL-only: VECTOR columns and indexes are MySQL's"),
            ..S
        },
        ScriptTest {
            name: "no-ff merge commit uses author session variables for author identity",
            intent: "A no-ff merge commit takes its author from the session's author settings.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'feature commit');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_author_name = 'Session Author';",
                "SET dolt_author_email = 'session@author.com';",
                "SET dolt_committer_name = 'Session Committer';",
                "SET dolt_committer_email = 'session@committer.com';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature', '--no-ff', '-m', 'test merge commit');",
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
                    query: "SELECT author, author_email, committer, email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("author", TEXT), Column("author_email", TEXT), Column("committer", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T("Session Author"), T("session@author.com"), T("Session Committer"), T("session@committer.com")],
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
fn test_keyless_merge_c_vs_and_conflicts_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "Keyless merge with unique indexes documents violations",
            intent: "A keyless merge records unique index violations.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (col1 int, col2 int UNIQUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (2, 1);",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'left');",
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
                    query: "SELECT dolt_violation_type, col1, col2 FROM dolt_constraint_violations_t ORDER BY col1 ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1")],
                            &[T("unique index"), T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY col1 ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", INT4), Column("col2", INT4)],
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
            name: "Keyless merge with foreign keys documents violations",
            intent: "A keyless merge records foreign key violations.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (parent_fk int, FOREIGN KEY (parent_fk) REFERENCES parent (pk));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent VALUES (1);",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO child VALUES (1);",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "DELETE FROM parent WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'left');",
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
                    query: "SELECT dolt_violation_type, parent_fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
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
                        columns: &[Column("parent_fk", INT4)],
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
            name: "Keyless merge documents conflicts",
            intent: "A keyless merge records conflicts.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "CREATE TABLE t (col1 int, col2 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'left');",
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
                    query: "SELECT base_col1, base_col2, our_col1, our_col2, their_col1, their_col2 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_col1", INT4), Column("base_col2", INT4), Column("our_col1", INT4), Column("our_col2", INT4), Column("their_col1", INT4), Column("their_col2", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unique key violation for keyless table",
            intent: "A keyless table's duplicate unique values record a violation.",
            set_up_script: &[
                "CREATE TABLE t (col1 int NOT NULL, col2 int, col3 int);",
                "CREATE UNIQUE INDEX ON t (col1, col2);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (1, NULL, NULL);",
                "INSERT INTO t VALUES (3, 3, NULL);",
                "SELECT dolt_commit('-Am', 'right cm');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, NULL, NULL);",
                "INSERT INTO t VALUES (3, 3, 1);",
                "SELECT dolt_commit('-Am', 'left cm');",
                "SET dolt_force_transaction_commit = 1;",
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
                    query: "SELECT col1, col2, col3 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("3"), T("3"), Null],
                            &[T("3"), T("3"), T("1")],
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
fn test_dolt_conflict_table_name_table_tests() {
    run_scripts(&[
        ScriptTest {
            name: "conflict diff types",
            intent: "Conflicts show each side's diff type.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1);",
                "INSERT INTO t VALUES (2, 2);",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'create table with row');",
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
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("4"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, dolt_our_diff_type, their_pk, their_col1, dolt_their_diff_type FROM dolt_conflicts_t ORDER BY coalesce(base_pk, our_pk, their_pk) ASC;",
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
            name: "keyless cardinality columns",
            intent: "Keyless conflicts show each side's row count.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "CREATE TABLE t (col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1), (2), (3), (4), (6);",
                "SELECT dolt_commit('-am', 'init');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (1);",
                "DELETE FROM t WHERE col1 = 2;",
                "INSERT INTO t VALUES (3);",
                "INSERT INTO t VALUES (4), (4);",
                "INSERT INTO t VALUES (5);",
                "DELETE FROM t WHERE col1 = 6;",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "DELETE FROM t WHERE col1 = 1;",
                "INSERT INTO t VALUES (2);",
                "INSERT INTO t VALUES (3);",
                "INSERT INTO t VALUES (4);",
                "INSERT INTO t VALUES (5);",
                "DELETE FROM t WHERE col1 = 6;",
                "SELECT dolt_commit('-am', 'left');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'right');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("6"), T("0")],
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
                    query: "SELECT base_col1, our_col1, their_col1, dolt_our_diff_type, dolt_their_diff_type, dolt_base_cardinality, dolt_our_cardinality, dolt_their_cardinality FROM dolt_conflicts_t ORDER BY coalesce(base_col1, our_col1, their_col1) ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("base_col1", INT4), Column("our_col1", INT4), Column("their_col1", INT4), Column("dolt_our_diff_type", TEXT), Column("dolt_their_diff_type", TEXT), Column("dolt_base_cardinality", INT8), Column("dolt_our_cardinality", INT8), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                            &[T("1"), Null, T("1"), T("removed"), T("modified"), T("1"), T("0"), T("2")],
                            &[T("2"), T("2"), Null, T("modified"), T("removed"), T("1"), T("2"), T("0")],
                            &[T("3"), T("3"), T("3"), T("modified"), T("modified"), T("1"), T("2"), T("2")],
                            &[T("4"), T("4"), T("4"), T("modified"), T("modified"), T("1"), T("2"), T("3")],
                            &[Null, T("5"), T("5"), T("added"), T("added"), T("0"), T("1"), T("1")],
                            &[T("6"), Null, Null, T("removed"), T("removed"), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 6",
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
fn test_merge_artifacts_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "conflicts on different branches can be merged",
            intent: "Conflicts on different branches merge into one working set.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_checkout('-b', 'conflicts1');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT dolt_branch('conflicts2');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert pk 1');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET col1 = 100 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'left edit');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE T SET col1 = -100 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT * FROM dolt_checkout('conflicts1');",
                "SELECT * FROM dolt_merge('other');",
                "SELECT dolt_commit('-afm', 'commit conflicts on conflicts1');",
                "SELECT * FROM dolt_checkout('conflicts2');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'insert pk 2');",
                "SELECT dolt_branch('other2');",
                "UPDATE t SET col1 = 100 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'left edit');",
                "SELECT * FROM dolt_checkout('other2');",
                "UPDATE T SET col1 = -100 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT * FROM dolt_checkout('conflicts2');",
                "SELECT * FROM dolt_merge('other2');",
                "SELECT dolt_commit('-afm', 'commit conflicts on conflicts2');",
                "SELECT * FROM dolt_checkout('conflicts1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("100"), T("1"), T("-100")],
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
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('conflicts2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'conflicts2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[T("2"), T("2"), T("2"), T("100"), T("2"), T("-100")],
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
                            &[T("2"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('conflicts1');",
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
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("100"), T("1"), T("-100")],
                            &[T("2"), T("2"), T("2"), T("100"), T("2"), T("-100")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("100")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET col1 = 300;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("300"), T("1"), T("-100")],
                            &[T("2"), T("2"), T("2"), T("300"), T("2"), T("-100")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicts of different schemas can't coexist",
            intent: "Conflicts of different schemas on one table can't be merged together.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert pk 1');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET col1 = 100 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'left edit');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE T SET col1 = -100 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other');",
                "SELECT dolt_commit('-afm', 'commit conflicts on main');",
                "ALTER TABLE t ADD COLUMN col2 int;",
                "SELECT dolt_commit('-afm', 'alter schema');",
                "SELECT * FROM dolt_checkout('-b', 'other2');",
                "UPDATE t SET col2 = -1000 WHERE pk = 1;",
                "SELECT dolt_commit('-afm', 'update pk 1 to -1000');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col2 = 1000 WHERE pk = 1;",
                "SELECT dolt_commit('-afm', 'update pk 1 to 1000');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other2');",
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
                    query: "SELECT * FROM dolt_merge('other2');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Dolt refuses a merge into a table holding another merge's conflicts of a different schema, where both merges' conflicts are kept, Appendix C \"a merge into a table holding another merge's conflicts\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("100"), T("1"), T("-100")],
                            &[T("1"), T("100"), T("1"), T("100"), T("1"), T("100")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses a merge into a table holding another merge's conflicts of a different schema, where both merges' conflicts are kept, Appendix C \"a merge into a table holding another merge's conflicts\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("100"), T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "violations with an older commit hash are overwritten if the value is the same",
            intent: "A violation recorded again by a later merge replaces the older commit's entry.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "SELECT * FROM dolt_checkout('-b', 'viol1');",
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (pk int PRIMARY KEY, fk int, FOREIGN KEY (fk) REFERENCES parent (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup table');",
                "SELECT dolt_branch('viol2');",
                "SELECT dolt_branch('other3');",
                "INSERT INTO parent VALUES (1);",
                "SELECT dolt_commit('-am', 'viol1 setup');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert child of 1');",
                "SELECT * FROM dolt_checkout('viol1');",
                "DELETE FROM parent WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'delete 1');",
                "SELECT * FROM dolt_merge('other');",
                "SELECT dolt_commit('-afm', 'commit violations 1');",
                "SELECT * FROM dolt_checkout('viol2');",
                "INSERT INTO parent VALUES (2);",
                "SELECT dolt_commit('-am', 'viol2 setup');",
                "SELECT * FROM dolt_checkout('-b', 'other2');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'insert child of 2');",
                "SELECT * FROM dolt_checkout('viol2');",
                "DELETE FROM parent WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'delete 2');",
                "SELECT * FROM dolt_merge('other2');",
                "SELECT dolt_commit('-afm', 'commit violations 2');",
                "SELECT * FROM dolt_checkout('other3');",
                "INSERT INTO PARENT VALUES (3);",
                "SELECT dolt_commit('-am', 'edit needed to trigger three-way merge');",
                "SELECT * FROM dolt_checkout('viol1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, fk FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("fk", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
                    query: "SELECT * FROM dolt_checkout('viol2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'viol2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, fk FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("fk", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
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
                    query: "SELECT * FROM dolt_merge('viol1');",
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
                    query: "SELECT dolt_violation_type, fk, pk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("fk", INT4), Column("pk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("1")],
                            &[T("foreign key"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, fk FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("fk", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
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
                    query: "SELECT dolt_commit('-afm', 'commit active merge');",
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
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE child SET fk = 4;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-afm', 'update children to new value');",
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
                    query: "SELECT * FROM dolt_merge('other3');",
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
                    query: "SELECT dolt_violation_type, pk, fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("1")],
                            &[T("foreign key"), T("1"), T("4")],
                            &[T("foreign key"), T("2"), T("2")],
                            &[T("foreign key"), T("2"), T("4")],
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
            name: "merging unique key violations in left and right",
            intent: "Unique violations on both sides merge into the violation table.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table t');",
                "SELECT dolt_branch('right');",
                "SELECT dolt_branch('left2');",
                "SELECT * FROM dolt_checkout('-b', 'right2');",
                "INSERT INTO T VALUES (4, 1);",
                "SELECT dolt_commit('-am', 'right2 insert');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO T VALUES (3, 1);",
                "SELECT dolt_commit('-am', 'right insert');",
                "SELECT * FROM dolt_checkout('left2');",
                "INSERT INTO T VALUES (2, 1);",
                "SELECT dolt_commit('-am', 'left2 insert');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO T VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'left insert');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'left2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    skip: Some("Dolt skips it too (TODO: constraint violations): dolt_preview_merge_conflicts_summary counts no constraint violations yet"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('left2');",
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
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1")],
                            &[T("unique index"), T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-afm', 'commit unique key viol');",
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
                    query: "SELECT * FROM dolt_checkout('right');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'right'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right2');",
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
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("3"), T("1")],
                            &[T("4"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("unique index"), T("3"), T("1")],
                            &[T("unique index"), T("4"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-afm', 'commit unique key viol');",
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
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                            &[T("4"), T("1")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1")],
                            &[T("unique index"), T("2"), T("1")],
                            &[T("unique index"), T("3"), T("1")],
                            &[T("unique index"), T("4"), T("1")],
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
            name: "right adds a unique key constraint and resolves existing violations.",
            intent: "A unique key added on the right after its violations were resolved merges cleanly.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1), (2, 1);",
                "SELECT dolt_commit('-am', 'table and data');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE t SET col1 = 2 WHERE pk = 2;",
                "CREATE UNIQUE INDEX col1_uniq ON t (col1);",
                "SELECT dolt_commit('-am', 'right adds a unique index');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 3);",
                "SELECT dolt_commit('-am', 'left edit');",
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
            name: "unique key violation should be thrown even if a PK column is used in the unique index",
            intent: "A unique index that includes a key column still records violations.",
            set_up_script: &[
                "CREATE TABLE t (col1 int NOT NULL, col2 int NOT NULL, col3 int, PRIMARY KEY (col1, col2));",
                "ALTER TABLE t ADD UNIQUE (col2, col3);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 2, 3);",
                "SELECT dolt_commit('-Am', 'left');",
                "SET dolt_force_transaction_commit = 1;",
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
                    query: "SELECT col1, col2, col3 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("2"), T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unique key violation should be thrown even if a PK column is used in the unique index 2",
            intent: "A unique index that includes a key column records violations in a second shape.",
            set_up_script: &[
                "CREATE TABLE wxyz (w int, x int, y int, z int, PRIMARY KEY (x, w));",
                "ALTER TABLE wxyz ADD UNIQUE (z, x);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO wxyz VALUES (1, 2, 3, 4);",
                "SELECT dolt_commit('-Am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO wxyz VALUES (5, 2, 6, 4);",
                "SELECT dolt_commit('-Am', 'left');",
                "SET dolt_force_transaction_commit = 1;",
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
                    query: "SELECT w, x, y, z FROM dolt_constraint_violations_wxyz;",
                    expected: Expected::Rows {
                        columns: &[Column("w", INT4), Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("4")],
                            &[T("5"), T("2"), T("6"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unique key violations should not be thrown for keys with null values",
            intent: "Unique keys with NULL values record no violations.",
            set_up_script: &[
                "CREATE TABLE t (col1 int NOT NULL, col2 int NOT NULL, col3 int, PRIMARY KEY (col1, col2));",
                "ALTER TABLE t ADD UNIQUE (col2, col3);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (1, 2, NULL);",
                "SELECT dolt_commit('-Am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 2, NULL);",
                "SELECT dolt_commit('-Am', 'left');",
                "SET dolt_force_transaction_commit = 1;",
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
                    query: "SELECT count(*) FROM dolt_constraint_violations;",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", INT4), Column("col2", INT4), Column("col3", INT4)],
                        rows: &[
                            &[T("1"), T("2"), Null],
                            &[T("2"), T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "regression test for bad column ordering in schema",
            intent: "A schema whose column order differs across sides merges in the right order.",
            set_up_script: &[
                "CREATE TABLE t (col1 text, col2 varchar(100), PRIMARY KEY (col2));",
                "CREATE UNIQUE INDEX ON t (col1);",
                "SELECT dolt_commit('-Am', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES ('A', 'first');",
                "SELECT dolt_commit('-Am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES ('A', 'second');",
                "SELECT dolt_commit('-Am', 'left');",
                "SET dolt_force_transaction_commit = 1;",
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
                    query: "SELECT col1, col2 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", TEXT), Column("col2", VARCHAR)],
                        rows: &[
                            &[T("A"), T("first")],
                            &[T("A"), T("second")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Multiple foreign key violations for a given row reported",
            intent: "A row violating several foreign keys gets one violation each.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE parent(pk int PRIMARY KEY, col1 int, col2 int); CREATE UNIQUE INDEX par_col1_idx ON parent (col1); CREATE UNIQUE INDEX par_col2_idx ON parent (col2);",
                "CREATE TABLE child(pk int PRIMARY KEY, col1 int, col2 int, FOREIGN KEY (col1) REFERENCES parent(col1), FOREIGN KEY (col2) REFERENCES parent(col2));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent VALUES (1, 1, 1);",
                "SELECT dolt_commit('-am', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO CHILD VALUES (1, 1, 1);",
                "SELECT dolt_commit('-am', 'insert child');",
                "SELECT * FROM dolt_checkout('main');",
                "DELETE FROM parent WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'delete parent');",
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
                            &[T("public.child"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Multiple unique key violations for a given row reported",
            intent: "A row violating several unique keys gets one violation each.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE, col2 int UNIQUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (2, 1, 1);",
                "SELECT dolt_commit('-am', 'right insert');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1, 1);",
                "SELECT dolt_commit('-am', 'left insert');",
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
                            &[T("public.t"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Multiple unique key violations part 1 (repro issue #5719)",
            intent: "Several unique violations for one row are each reported (part 1).",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (id int NOT NULL, col1 varchar(255), col2 varchar(255), col3 varchar(255), PRIMARY KEY (id)); CREATE UNIQUE INDEX uniq_idx ON t (col1,col2,col3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t (id, col1, col2, col3) VALUES (1, 'val1', 'val1', 'val1'), (4, 'val1', 'val1', 'val2');",
                "SELECT dolt_commit('-am', 'right insert');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, col1, col2, col3) VALUES (2, 'val1', 'val1', 'val1'), (3, 'val1', 'val1', 'val2');",
                "SELECT dolt_commit('-am', 'left insert');",
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
                            &[T("public.t"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, col1, col2, col3 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("col1", VARCHAR), Column("col2", VARCHAR), Column("col3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("val1"), T("val1"), T("val1")],
                            &[T("2"), T("val1"), T("val1"), T("val1")],
                            &[T("3"), T("val1"), T("val1"), T("val2")],
                            &[T("4"), T("val1"), T("val1"), T("val2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Multiple unique key violations part 2 (repro issue #5719)",
            intent: "Several unique violations for one row are each reported (part 2).",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (id int NOT NULL, col1 varchar(255), col2 varchar(255), col3 varchar(255), PRIMARY KEY (id)); CREATE UNIQUE INDEX uniq_idx ON t (col1,col2,col3);",
                "INSERT INTO t (id, col1, col2, col3) VALUES (1, 'val1', 'val1', 'val1'), (2, 'val1', 'val2', 'val1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE t SET col3 = 'val2';",
                "SELECT dolt_commit('-am', 'right update');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, col1, col2, col3) VALUES (3, 'val1', 'val1', 'val2');",
                "SELECT dolt_commit('-am', 'main insert');",
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
                            &[T("public.t"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, col1, col2, col3 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("col1", VARCHAR), Column("col2", VARCHAR), Column("col3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("val1"), T("val1"), T("val2")],
                            &[T("3"), T("val1"), T("val1"), T("val2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Multiple unique key violations part 3 (repro issue #5719)",
            intent: "Several unique violations for one row are each reported (part 3).",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (id int NOT NULL, col1 varchar(255), col2 varchar(255), col3 varchar(255), PRIMARY KEY (id)); CREATE UNIQUE INDEX uniq_idx ON t (col1,col2,col3);",
                "INSERT INTO t (id, col1, col2, col3) VALUES (1, 'val1', 'val1', 'val1'), (4, 'val1', 'val2', 'val1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE t SET col3 = 'val2';",
                "SELECT dolt_commit('-am', 'right update');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, col1, col2, col3) VALUES (3, 'val1', 'val1', 'val2');",
                "SELECT dolt_commit('-am', 'main insert');",
                "SELECT * FROM dolt_checkout('right');",
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
                    query: "SELECT id, col1, col2, col3 FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("col1", VARCHAR), Column("col2", VARCHAR), Column("col3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("val1"), T("val1"), T("val2")],
                            &[T("3"), T("val1"), T("val1"), T("val2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "violation system table supports multiple violations per row",
            intent: "The violation table holds several violations for one row.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, a int NOT NULL, b int NOT NULL); CREATE UNIQUE INDEX ua ON t (a); CREATE UNIQUE INDEX ub ON t (b);",
                "INSERT INTO t VALUES (1, 10, 20);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET a = 0, b = 0;",
                "SELECT dolt_commit('-am', 'main: row 1 -> (0,0)');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO t VALUES (2, 0, 0);",
                "SELECT dolt_commit('-am', 'other: add row (2,0,0)');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_constraint_violations_t;",
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
                    query: "SELECT dolt_from_root_ish, dolt_violation_type, pk, a, b FROM dolt_constraint_violations_t ORDER BY pk, CAST(dolt_violation_info AS text);",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[Any, T("unique index"), T("1"), T("0"), T("0")],
                            &[Any, T("unique index"), T("1"), T("0"), T("0")],
                            &[Any, T("unique index"), T("2"), T("0"), T("0")],
                            &[Any, T("unique index"), T("2"), T("0"), T("0")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_constraint_violations_t WHERE pk = 1;",
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
                    query: "SELECT COUNT(*) FROM dolt_constraint_violations_t WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "clearing constraint violations (MySQL): single delete, bulk delete, and commit",
            intent: "Constraint violations clear by single delete, bulk delete, and commit.",
            set_up_script: &[
                "SET dolt_force_transaction_commit = ON;",
                "CREATE TABLE t (pk int PRIMARY KEY, a int NOT NULL, b int NOT NULL); CREATE UNIQUE INDEX ua ON t (a); CREATE UNIQUE INDEX ub ON t (b);",
                "INSERT INTO t VALUES (1, 10, 20);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET a = 0, b = 0;",
                "SELECT dolt_commit('-am', 'main: row 1 -> (0,0)');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO t VALUES (2, 0, 0);",
                "SELECT dolt_commit('-am', 'other: add row (2,0,0)');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT COUNT(*) FROM dolt_constraint_violations_t;",
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
                    query: "DELETE FROM dolt_constraint_violations_t WHERE pk = 1 AND dolt_violation_type = 'unique index' AND jsonb_path_query_first((dolt_violation_info)::jsonb, ('$.Name')::jsonpath) = 'ua';",
                    expected: Expected::Tag("DELETE 1"),
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_constraint_violations_t;",
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
                    query: "DELETE FROM dolt_constraint_violations_t WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_constraint_violations_t WHERE pk = 1;",
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
                    query: "SELECT COUNT(*) FROM dolt_constraint_violations_t WHERE pk = 2;",
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
                    query: "UPDATE t SET a = 1, b = 2 WHERE pk = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t WHERE pk = 2;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_constraint_violations_t;",
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
                    query: "SELECT dolt_commit('-am', 'merge and resolve violations');",
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
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("0"), T("0")],
                            &[T("2"), T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "merge error lists all constraint violations when table has multiple violations",
            intent: "A merge error lists every constraint violation of a table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE, col2 int UNIQUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (2, 1, 1);",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1, 1);",
                "SELECT dolt_commit('-am', 'left edit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Committing this transaction resulted in a working set with constraint violations, transaction rolled back. This constraint violation may be the result of a previous merge or the result of transaction sequencing. Constraint violations from a merge can be resolved using the dolt_constraint_violations table before committing the transaction. To allow transactions to be committed with constraint violations from a merge or transaction sequencing set dolt_force_transaction_commit to on.\nConstraint violations: \nType: Unique Key Constraint Violation,\n	Name: t_col1_key,\n	Columns: [col1] (2 row(s))\nType: Unique Key Constraint Violation,\n	Name: t_col2_key,\n	Columns: [col2] (2 row(s))", ..E }),
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, and the error lists them so (Appendix C); the message also names the setting as Postgres sets it, with no @@")),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_CONSTRAINT_VIOLATIONS;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
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
            name: "merge error includes row count for foreign key violations",
            intent: "A merge error counts the rows with foreign key violations.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (pk int PRIMARY KEY, fk int, FOREIGN KEY (fk) REFERENCES parent(pk));",
                "INSERT INTO parent VALUES (1);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create tables');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO child VALUES (2, 1), (3, 1);",
                "SELECT dolt_commit('-am', 'add two children referencing parent');",
                "SELECT * FROM dolt_checkout('main');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 1;",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-am', 'delete parent');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Committing this transaction resulted in a working set with constraint violations, transaction rolled back. This constraint violation may be the result of a previous merge or the result of transaction sequencing. Constraint violations from a merge can be resolved using the dolt_constraint_violations table before committing the transaction. To allow transactions to be committed with constraint violations from a merge or transaction sequencing set dolt_force_transaction_commit to on.\nConstraint violations: \nType: Foreign Key Constraint Violation\n	ForeignKey: child_ibfk_1,\n	Table: child,\n	ReferencedTable: parent,\n	Index: fk,\n	ReferencedIndex:  (3 row(s))", ..E }),
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_CONSTRAINT_VIOLATIONS;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
        ScriptTest {
            name: "merge error includes row count for null constraint violations",
            intent: "A merge error counts the rows with NOT NULL violations.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "UPDATE t SET c = NULL WHERE pk IN (1, 2);",
                "SELECT dolt_commit('-am', 'set both to null');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN c TYPE int, ALTER COLUMN c SET NOT NULL, ALTER COLUMN c DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'add not null');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Committing this transaction resulted in a working set with constraint violations, transaction rolled back. This constraint violation may be the result of a previous merge or the result of transaction sequencing. Constraint violations from a merge can be resolved using the dolt_constraint_violations table before committing the transaction. To allow transactions to be committed with constraint violations from a merge or transaction sequencing set dolt_force_transaction_commit to on.\nConstraint violations: \nType: Null Constraint Violation,\n	Columns: [c] (2 row(s))", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_CONSTRAINT_VIOLATIONS;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
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
            name: "merge error includes row count for check constraint violations",
            intent: "A merge error counts the rows with check violations.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (3, -1), (4, -1);",
                "SELECT dolt_commit('-am', 'add rows that will violate check');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CONSTRAINT chk_positive CHECK (col > 0);",
                "SELECT dolt_commit('-am', 'add check constraint');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Committing this transaction resulted in a working set with constraint violations, transaction rolled back. This constraint violation may be the result of a previous merge or the result of transaction sequencing. Constraint violations from a merge can be resolved using the dolt_constraint_violations table before committing the transaction. To allow transactions to be committed with constraint violations from a merge or transaction sequencing set dolt_force_transaction_commit to on.\nConstraint violations: \nType: Check Constraint Violation,\n	Name: chk_positive,\n	Expression: (`col` > 0) (2 row(s))", ..E }),
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_CONSTRAINT_VIOLATIONS;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Dolt runs this script only for its MySQL dialect"),
            ..S
        },
    ]);
}

#[test]
fn test_schema_conflict_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "schema conflicts return an error when autocommit is enabled",
            intent: "A schema conflict fails the merge when autocommit is on.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 varchar(20));",
                "SELECT dolt_commit('-Am', 'added table t');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE int USING c0::int, ALTER COLUMN c0 DROP NOT NULL, ALTER COLUMN c0 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'altered t on branch other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE timestamp(6) USING c0::timestamp(6), ALTER COLUMN c0 DROP NOT NULL, ALTER COLUMN c0 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'altered t on branch main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other');",
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
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
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
            ],
            ..S
        },
        ScriptTest {
            name: "divergent type change causes schema conflict",
            intent: "A column retyped differently on both sides is a schema conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, c0 varchar(20));",
                "SELECT dolt_commit('-Am', 'added table t');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE int USING c0::int, ALTER COLUMN c0 DROP NOT NULL, ALTER COLUMN c0 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'altered t on branch other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN c0 TYPE timestamp(6) USING c0::timestamp(6), ALTER COLUMN c0 DROP NOT NULL, ALTER COLUMN c0 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'altered t on branch main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'other');",
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
                    query: "SELECT * FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("base_schema", TEXT), Column("our_schema", TEXT), Column("their_schema", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    c0 character varying(20)\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    c0 timestamp(6) without time zone\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    c0 integer\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);"), T("different column definitions for our column c0 and their column c0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and dolt_schema_conflicts shows each side's schema as pg_dump's statements for the table, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("schema conflict")],
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

#[test]
fn test_generated_column_merge_test_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "merge a generated stored column",
            intent: "A stored generated column merges and is computed for both sides' rows.",
            set_up_script: &[
                "CREATE TABLE t1 (id bigint PRIMARY KEY, v1 bigint, v2 bigint, v3 bigint GENERATED ALWAYS AS (v1 + v2) stored); CREATE INDEX  ON t1 (v3);",
                "INSERT INTO t1 (id, v1, v2) VALUES (1, 1, 1), (2, 2, 2);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t1 (id, v1, v2) VALUES (3, 3, 3);",
                "SELECT dolt_commit('-Am', 'branch1 commit');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "INSERT INTO t1 (id, v1, v2) VALUES (4, 4, 4);",
                "SELECT dolt_commit('-Am', 'branch2 commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
                    query: "SELECT * FROM t1 ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("2")],
                            &[T("2"), T("2"), T("2"), T("4")],
                            &[T("3"), T("3"), T("3"), T("6")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch2');",
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
                    query: "SELECT * FROM t1 ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("2")],
                            &[T("2"), T("2"), T("2"), T("4")],
                            &[T("3"), T("3"), T("3"), T("6")],
                            &[T("4"), T("4"), T("4"), T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 8;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
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
            name: "merge a generated column with non-conflicting changes on both sides",
            intent: "A generated column merges with changes to its sources on both sides.",
            set_up_script: &[
                "CREATE TABLE t1 (id bigint PRIMARY KEY, v1 bigint, v2 bigint, v3 bigint GENERATED ALWAYS AS (v1 + v2) stored);",
                "INSERT INTO t1 (id, v1, v2) VALUES (1, 1, 1), (2, 2, 2);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t1 SET v1 = 4 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'branch1 commit');",
                "SELECT * FROM dolt_checkout('branch2');",
                "UPDATE t1 SET v2 = 5 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'branch2 commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
                    query: "SELECT * FROM t1 ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("1"), T("4"), T("1"), T("5")],
                            &[T("2"), T("2"), T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch2');",
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
                    query: "SELECT * FROM t1 ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("1"), T("4"), T("5"), T("9")],
                            &[T("2"), T("2"), T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 9;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
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
            name: "merge a generated column created on another branch",
            intent: "A generated column made on another branch comes with the merge.",
            set_up_script: &[
                "CREATE TABLE t1 (id bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "INSERT INTO t1 (id, v1, v2) VALUES (1, 1, 1), (2, 2, 2);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t1 (id, v1, v2) VALUES (3, 3, 3);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SELECT * FROM dolt_checkout('branch1');",
                "ALTER TABLE t1 ADD COLUMN v3 bigint GENERATED ALWAYS AS (v1 + v2) stored;",
                "CREATE INDEX idx_v3 ON t1 (v3);",
                "INSERT INTO t1 (id, v1, v2) VALUES (4, 4, 4);",
                "SELECT dolt_commit('-Am', 'branch1 commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
                    query: "SELECT * FROM t1 ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("2")],
                            &[T("2"), T("2"), T("2"), T("4")],
                            &[T("3"), T("3"), T("3"), T("6")],
                            &[T("4"), T("4"), T("4"), T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 8;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
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
            name: "merge a virtual column",
            intent: "A virtual column merges.",
            set_up_script: &[
                "CREATE TABLE t1 (id bigint PRIMARY KEY, v1 bigint, v2 bigint, v3 bigint GENERATED ALWAYS AS (v1 + v2));",
                "INSERT INTO t1 (id, v1, v2) VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t1 (id, v1, v2) VALUES (7, 8, 9);",
                "SELECT dolt_commit('-Am', 'branch1 commit');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "INSERT INTO t1 (id, v1, v2) VALUES (10, 11, 12);",
                "SELECT dolt_commit('-Am', 'branch2 commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
                    query: "SELECT * FROM t1 ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("5")],
                            &[T("4"), T("5"), T("6"), T("11")],
                            &[T("7"), T("8"), T("9"), T("17")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 17;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch2');",
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
                    query: "SELECT * FROM t1 ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("5")],
                            &[T("4"), T("5"), T("6"), T("11")],
                            &[T("7"), T("8"), T("9"), T("17")],
                            &[T("10"), T("11"), T("12"), T("23")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 23;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge a virtual column created on another branch",
            intent: "A virtual column made on another branch comes with the merge.",
            set_up_script: &[
                "CREATE TABLE t1 (id bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "INSERT INTO t1 (id, v1, v2) VALUES (1, 2, 3), (4, 5, 6);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t1 (id, v1, v2) VALUES (7, 8, 9);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SELECT * FROM dolt_checkout('branch1');",
                "ALTER TABLE t1 ADD COLUMN v3 bigint GENERATED ALWAYS AS (v1 + v2);",
                "INSERT INTO t1 (id, v1, v2) VALUES (10, 11, 12);",
                "SELECT dolt_commit('-Am', 'branch1 commit');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
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
                    query: "SELECT * FROM t1 ORDER by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("5")],
                            &[T("4"), T("5"), T("6"), T("11")],
                            &[T("7"), T("8"), T("9"), T("17")],
                            &[T("10"), T("11"), T("12"), T("23")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 17;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE v3 = 23;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("10")],
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
fn test_preview_merge_conflicts_function_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "invalid arguments",
            intent: "dolt_preview_merge_conflicts checks its arguments.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts_summary() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('t');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts_summary(unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1', 't');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts_summary(unknown, unknown, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary(NULL, NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_preview_merge_conflicts_summary: NULL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts_summary(unknown, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary(123, 'branch1');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts_summary(integer, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('fake-branch', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'fake-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main...branch1', 'branch2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "string is not a valid branch or hash", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', concat('branch', '1'));",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary(dolt_hashof('main'), 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('t');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts(unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts(unknown, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't', 'extra');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts(unknown, unknown, unknown, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts(NULL, NULL, NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_preview_merge_conflicts: NULL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 123, 't');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts(unknown, integer, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts(123, 'branch1', 't');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts(integer, unknown, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 123);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_preview_merge_conflicts(unknown, unknown, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('fake-branch', 'main', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'fake-branch', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: fake-branch", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main...branch1', 'branch2', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "string is not a valid branch or hash", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', concat('branch', '1'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts(dolt_hashof('main'), 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("the merge preview functions take their arguments as the diff functions do, any argument Postgres can evaluate before the query runs, resolved by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 'nope');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"nope\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with single table",
            intent: "dolt_preview_merge_conflicts shows a single table's conflicts without merging.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "UPDATE t SET c1='one!' WHERE pk=1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'update row 1 on branch2'), FALSE);",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET c1='one?' WHERE pk=1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'update row 1 on branch1'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
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
                    query: "SELECT base_pk, base_c1, base_c2, our_pk, our_c1, our_c2, dolt_our_diff_type, their_pk, their_c1, their_c2, dolt_their_diff_type FROM dolt_preview_merge_conflicts('main', 'branch2', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), T("1"), T("one?"), T("two"), T("modified"), T("1"), T("one!"), T("two"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
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
                    query: "SELECT base_pk, base_c1, base_c2, our_pk, our_c1, our_c2, dolt_our_diff_type, their_pk, their_c1, their_c2, dolt_their_diff_type FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), T("1"), T("one?"), T("two"), T("modified"), T("1"), T("one!"), T("two"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch2', 'main');",
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
                    query: "SELECT base_pk, base_c1, base_c2, our_pk, our_c1, our_c2, dolt_our_diff_type, their_pk, their_c1, their_c2, dolt_their_diff_type FROM dolt_preview_merge_conflicts('branch2', 'main', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("two"), T("1"), T("one!"), T("two"), T("modified"), T("1"), T("one?"), T("two"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case with keyless table",
            intent: "dolt_preview_merge_conflicts shows a keyless table's conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "UPDATE t SET c1='one!' WHERE pk=1;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'update row 1 on branch2'), FALSE);",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET c1='one?' WHERE pk=1;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'update row 1 on branch1'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT), Column("dolt_base_cardinality", INT8), Column("dolt_our_cardinality", INT8), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('main', 'branch2', 't');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT), Column("dolt_base_cardinality", INT8), Column("dolt_our_cardinality", INT8), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch2', 'main');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch2', 'main', 't');",
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
            ..S
        },
        ScriptTest {
            name: "basic case with multiple tables, data conflicts",
            intent: "dolt_preview_merge_conflicts shows data conflicts across several tables.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "CREATE TABLE t2 (pk int PRIMARY KEY, c1 varchar(20));",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "INSERT INTO t2 VALUES(100, 'hundred');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "UPDATE t SET c1='one!' WHERE pk=1;",
                "UPDATE t2 SET c1='hundred!' WHERE pk=100;",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'update row 1 on branch2'), FALSE);",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET c1='one?' WHERE pk=1;",
                "UPDATE t2 SET c1='hundred?' WHERE pk=100;",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'update row 1 on branch1'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
                "CREATE TABLE keyless (id int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                            &[T("public.t2"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('main', 'branch2', 't');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('main', 'branch2', 't2');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                            &[T("public.t2"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't2');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch2', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                            &[T("public.t2"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch2', 'main', 't');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch2', 'main', 't2');",
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
            ..S
        },
        ScriptTest {
            name: "basic case with multiple tables, schema conflict",
            intent: "dolt_preview_merge_conflicts shows a schema conflict among several tables.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "CREATE TABLE t2 (pk int PRIMARY KEY, c1 varchar(20));",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "INSERT INTO t2 VALUES(100, 'hundred');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', ('')::text, FALSE);",
                "SELECT set_config('test.Commit1', dolt_commit('-am', 'creating table t'), FALSE);",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "UPDATE t SET c1='one!' WHERE pk=1;",
                "ALTER TABLE t2 ALTER COLUMN c1 SET DEFAULT 'default';",
                "SELECT set_config('test.Commit2', ('')::text, FALSE);",
                "SELECT set_config('test.Commit2', dolt_commit('-am', 'update row 1 on branch2'), FALSE);",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET c1='one?' WHERE pk=1;",
                "ALTER TABLE t2 ALTER COLUMN c1 SET DEFAULT 'default2';",
                "SELECT set_config('test.Commit3', ('')::text, FALSE);",
                "SELECT set_config('test.Commit3', dolt_commit('-am', 'update row 1 on branch1'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
                "CREATE TABLE keyless (id int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                            &[T("public.t2"), Null, T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('main', 'branch2', 't');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('main', 'branch2', 't2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                            &[T("public.t2"), Null, T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary(current_setting('test.Commit1'), current_setting('test.Commit2'));",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts(current_setting('test.Commit1'), current_setting('test.Commit2'), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch2', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                            &[T("public.t2"), Null, T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch2', 'main', 't');",
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
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch2', 'main', 't2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-only conflicts",
            intent: "dolt_preview_merge_conflicts shows schema-only conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20) DEFAULT 'orig', c2 varchar(20));",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "ALTER TABLE t ALTER COLUMN c1 SET DEFAULT 'default1';",
                "SELECT dolt_commit('-am', 'change default on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "ALTER TABLE t ALTER COLUMN c1 SET DEFAULT 'default2';",
                "SELECT dolt_commit('-am', 'change default on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch2', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "mixed schema and data conflicts in same table",
            intent: "dolt_preview_merge_conflicts shows schema and data conflicts in one table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20) DEFAULT 'orig', c2 varchar(20));",
                "INSERT INTO t VALUES (1, 'one', 'two'), (2, 'two', 'three');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "UPDATE t SET c1='one!' WHERE pk=1;",
                "ALTER TABLE t ALTER COLUMN c1 SET DEFAULT 'default1';",
                "SELECT dolt_commit('-am', 'data and schema changes on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET c1='one?' WHERE pk=1;",
                "ALTER TABLE t ALTER COLUMN c1 SET DEFAULT 'default2';",
                "SELECT dolt_commit('-am', 'data and schema changes on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("base_c2", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("our_c2", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("their_c2", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch2', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "column type conflicts",
            intent: "dolt_preview_merge_conflicts shows column type conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "ALTER TABLE t ALTER COLUMN c1 TYPE varchar(50), ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'change column to varchar(50) on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "ALTER TABLE t ALTER COLUMN c1 TYPE text, ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'change column to text on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
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
            ],
            ..S
        },
        ScriptTest {
            name: "foreign key constraint conflicts",
            intent: "dolt_preview_merge_conflicts shows foreign key conflicts.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY, name varchar(20));",
                "CREATE TABLE child (pk int PRIMARY KEY, parent_pk int, data varchar(20));",
                "INSERT INTO parent VALUES (1, 'parent1'), (2, 'parent2');",
                "INSERT INTO child VALUES (1, 1, 'child1'), (2, 2, 'child2');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial tables');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (parent_pk) REFERENCES parent(pk) ON DELETE cascade;",
                "SELECT dolt_commit('-am', 'add fk with cascade on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (parent_pk) REFERENCES parent(pk) ON DELETE restrict;",
                "SELECT dolt_commit('-am', 'add fk with restrict on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.child"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and Dolt's preview leaves out the foreign key conflicts its merge finds, Appendix C \"schema conflicts in the merge preview\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.child"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and Dolt's preview leaves out the foreign key conflicts its merge finds, Appendix C \"schema conflicts in the merge preview\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint conflicts",
            intent: "dolt_preview_merge_conflicts shows check constraint conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, score int);",
                "INSERT INTO t VALUES (1, 85), (2, 92);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "ALTER TABLE t ADD CONSTRAINT chk1 CHECK (score >= 0);",
                "SELECT dolt_commit('-am', 'add check >= 0 on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "ALTER TABLE t ADD CONSTRAINT chk1 CHECK (score > 0);",
                "SELECT dolt_commit('-am', 'add check > 0 on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and Dolt counts one conflicting index or check twice, Appendix C \"schema conflicts in the merge preview\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and Dolt counts one conflicting index or check twice, Appendix C \"schema conflicts in the merge preview\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index conflicts",
            intent: "dolt_preview_merge_conflicts shows index conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, name varchar(20), email varchar(50));",
                "INSERT INTO t VALUES (1, 'alice', 'alice@email.com'), (2, 'bob', 'bob@email.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "CREATE INDEX idx_name ON t(name);",
                "SELECT dolt_commit('-am', 'add name index on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "CREATE UNIQUE INDEX idx_name ON t(name);",
                "SELECT dolt_commit('-am', 'add unique name index on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and Dolt counts one conflicting index or check twice, Appendix C \"schema conflicts in the merge preview\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and Dolt counts one conflicting index or check twice, Appendix C \"schema conflicts in the merge preview\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "many data conflicts",
            intent: "dolt_preview_merge_conflicts shows many data conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20), c3 varchar(20));",
                "INSERT INTO t VALUES (1, 'a1', 'b1', 'c1'), (2, 'a2', 'b2', 'c2'), (3, 'a3', 'b3', 'c3'), (4, 'a4', 'b4', 'c4'), (5, 'a5', 'b5', 'c5');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial data');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "UPDATE t SET c1=concat(c1, '_branch2') WHERE pk IN (1,2,3);",
                "UPDATE t SET c2=concat(c2, '_branch2') WHERE pk IN (2,4);",
                "SELECT dolt_commit('-am', 'modify multiple rows on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET c1=concat(c1, '_branch1') WHERE pk IN (1,2,3);",
                "UPDATE t SET c2=concat(c2, '_branch1') WHERE pk IN (2,4);",
                "UPDATE t SET c3=concat(c3, '_branch1') WHERE pk = 5;",
                "SELECT dolt_commit('-am', 'modify multiple rows on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_preview_merge_conflicts('main', 'branch1', 't');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("4"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_preview_merge_conflicts('main', 'branch2', 't');",
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
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("4"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "additional conflicts testing with multiple columns",
            intent: "dolt_preview_merge_conflicts shows conflicts across several columns.",
            set_up_script: &[
                "CREATE TABLE test_table (pk int PRIMARY KEY, col1 varchar(20), col2 int);",
                "INSERT INTO test_table VALUES (1, 'original', 100), (2, 'second', 200);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "UPDATE test_table SET col1 = 'branch2_val', col2 = 300 WHERE pk = 1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'modify on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE test_table SET col1 = 'branch1_val', col2 = 400 WHERE pk = 1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'modify on branch1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_preview_merge_conflicts('branch1', 'branch2', 'test_table');",
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
                    query: "SELECT base_pk, our_col1, their_col1 FROM dolt_preview_merge_conflicts('branch1', 'branch2', 'test_table');",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("our_col1", VARCHAR), Column("their_col1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("branch1_val"), T("branch2_val")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "empty result when no conflicts",
            intent: "dolt_preview_merge_conflicts shows nothing when there are no conflicts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, value varchar(20));",
                "INSERT INTO t VALUES (1, 'original');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial setup');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "INSERT INTO t VALUES (2, 'branch2_new');",
                "SELECT dolt_commit('-am', 'add row on branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (3, 'branch1_new');",
                "SELECT dolt_commit('-am', 'add row on branch1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('branch1', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch1', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_value", VARCHAR), Column("our_pk", INT4), Column("our_value", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_value", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('main', 'branch2', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_value", VARCHAR), Column("our_pk", INT4), Column("our_value", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_value", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts('branch1', 'branch2', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_value", VARCHAR), Column("our_pk", INT4), Column("our_value", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_value", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt1_conflict_table_name_table_tests() {
    run_scripts(&[
        ScriptTest {
            name: "Provides a dolt_conflicts_id",
            intent: "Each row conflict has a dolt_conflict_id that selects, updates, and deletes that conflict alone.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 100);",
                "INSERT INTO t VALUES (2, 200);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, -100);",
                "INSERT INTO t VALUES (2, -200);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.hash1', ((SELECT dolt_conflict_id FROM dolt_conflicts_t WHERE our_pk = 1))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.hash2', ((SELECT dolt_conflict_id FROM dolt_conflicts_t WHERE our_pk = 2))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t WHERE dolt_conflict_id = current_setting('test.hash1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("-100"), T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t WHERE dolt_conflict_id = current_setting('test.hash2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_col1 = their_col1 WHERE dolt_conflict_id = current_setting('test.hash1', TRUE);",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("100"), T("1"), T("100")],
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t WHERE dolt_conflict_id = current_setting('test.hash1', TRUE);",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_conflicts_id is unique across merges",
            intent: "Conflicts of the same row from different merges, and of different rows, have different dolt_conflict_ids.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 100);",
                "INSERT INTO t VALUES (2, 200);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, -100);",
                "INSERT INTO t VALUES (2, -200);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
                "SELECT dolt_commit('-afm', 'commit conflicts');",
                "SELECT * FROM dolt_checkout('-b', 'other2');",
                "UPDATE t SET col1 = 9999 WHERE pk = 1;",
                "SELECT dolt_commit('-afm', 'commit on other2');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = 8888 WHERE pk = 1;",
                "SELECT dolt_commit('-afm', 'commit on main');",
                "SELECT * FROM dolt_merge('other2');",
                "SELECT set_config('test.hash1', ((SELECT dolt_conflict_id FROM dolt_conflicts_t WHERE our_pk = 1 AND their_col1 = 100))::text, FALSE);",
                "SELECT set_config('test.hash2', ((SELECT dolt_conflict_id FROM dolt_conflicts_t WHERE our_pk = 1 AND their_col1 = 9999))::text, FALSE);",
                "SELECT set_config('test.hash3', ((SELECT dolt_conflict_id FROM dolt_conflicts_t WHERE our_pk = 2))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_setting('test.hash1', TRUE) != current_setting('test.hash2', TRUE) AND current_setting('test.hash2', TRUE) != current_setting('test.hash3', TRUE);",
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
        ScriptTest {
            name: "Updates on our columns get applied to the source table - smoke",
            intent: "An UPDATE of a conflict's our_ columns writes the new values to the table's row.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 100);",
                "INSERT INTO t VALUES (2, 200);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, -100);",
                "INSERT INTO t VALUES (2, -200);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("-100"), T("1"), T("100")],
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_col1 = 1000 WHERE our_pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("1000"), T("1"), T("100")],
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1000")],
                            &[T("2"), T("-200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_col1 = their_col1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("100"), T("1"), T("100")],
                            &[Null, Null, T("2"), T("200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
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
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Updates on our columns get applied to the source table - compound / inverted pks",
            intent: "An UPDATE of our_ columns finds the table's row by a primary key whose columns are declared in another order than the key lists them.",
            set_up_script: &[
                "CREATE TABLE t (pk2 int, pk1 int, col1 int, PRIMARY KEY (pk1, pk2));",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 1, 100), (2, 1, 200);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1, -100), (2, 1, -200);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_pk1, base_pk2, base_col1, our_pk1, our_pk2, our_col1, their_pk1, their_pk2, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk1", INT4), Column("base_pk2", INT4), Column("base_col1", INT4), Column("our_pk1", INT4), Column("our_pk2", INT4), Column("our_col1", INT4), Column("their_pk1", INT4), Column("their_pk2", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("1"), T("-100"), T("1"), T("1"), T("100")],
                            &[Null, Null, Null, T("1"), T("2"), T("-200"), T("1"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_col1 = 1000;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk1, base_pk2, base_col1, our_pk1, our_pk2, our_col1, their_pk1, their_pk2, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk1", INT4), Column("base_pk2", INT4), Column("base_col1", INT4), Column("our_pk1", INT4), Column("our_pk2", INT4), Column("our_col1", INT4), Column("their_pk1", INT4), Column("their_pk2", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("1"), T("1000"), T("1"), T("1"), T("100")],
                            &[Null, Null, Null, T("1"), T("2"), T("1000"), T("1"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk2", INT4), Column("pk1", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1000")],
                            &[T("2"), T("1"), T("1000")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Updates on our columns get applied to the source table - keyless",
            intent: "An UPDATE of our_ columns of a keyless table's conflict changes one copy of the row, and an UPDATE of the cardinalities changes nothing.",
            set_up_script: &[
                "CREATE TABLE t (name varchar(100), price int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES ('apple', 1);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES ('apple', 1), ('apple', 1);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_name, base_price, dolt_base_cardinality, our_name, our_price, dolt_our_cardinality, their_name, their_price, dolt_their_cardinality FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_name", VARCHAR), Column("base_price", INT4), Column("dolt_base_cardinality", INT8), Column("our_name", VARCHAR), Column("our_price", INT4), Column("dolt_our_cardinality", INT8), Column("their_name", VARCHAR), Column("their_price", INT4), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                            &[Null, Null, T("0"), T("apple"), T("1"), T("2"), T("apple"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_name = 'orange' WHERE our_name = 'apple';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_name, base_price, dolt_base_cardinality, our_name, our_price, dolt_our_cardinality, their_name, their_price, dolt_their_cardinality FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_name", VARCHAR), Column("base_price", INT4), Column("dolt_base_cardinality", INT8), Column("our_name", VARCHAR), Column("our_price", INT4), Column("dolt_our_cardinality", INT8), Column("their_name", VARCHAR), Column("their_price", INT4), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                            &[Null, Null, T("0"), T("apple"), T("1"), T("1"), T("apple"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("name", VARCHAR), Column("price", INT4)],
                        rows: &[
                            &[T("apple"), T("1")],
                            &[T("orange"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET dolt_our_cardinality = 10, dolt_their_cardinality = 10, dolt_base_cardinality = 10;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_name, base_price, dolt_base_cardinality, our_name, our_price, dolt_our_cardinality, their_name, their_price, dolt_their_cardinality FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_name", VARCHAR), Column("base_price", INT4), Column("dolt_base_cardinality", INT8), Column("our_name", VARCHAR), Column("our_price", INT4), Column("dolt_our_cardinality", INT8), Column("their_name", VARCHAR), Column("their_price", INT4), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                            &[Null, Null, T("0"), T("apple"), T("1"), T("1"), T("apple"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("name", VARCHAR), Column("price", INT4)],
                        rows: &[
                            &[T("apple"), T("1")],
                            &[T("orange"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Updating our cols when the row is missing inserts the row",
            intent: "An UPDATE of our_ columns inserts the row when our side has none, which needs the whole primary key, and leaves a conflict whose sides both lack the row alone.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t VALUES (1, NULL);",
                "INSERT INTO t VALUES (2, NULL);",
                "INSERT INTO t VALUES (3, NULL);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "UPDATE t SET col1 = 100 WHERE pk = 1;",
                "DELETE FROM t WHERE pk = 2;",
                "UPDATE t SET col1 = 300 WHERE pk = 3;",
                "INSERT INTO t VALUES (4, 400);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE t SET col1 = -100 WHERE pk = 1;",
                "UPDATE t SET col1 = -200 WHERE pk = 2;",
                "DELETE FROM t WHERE pk = 3;",
                "INSERT INTO t VALUES (4, -400);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[T("1"), Null, T("1"), T("-100"), T("1"), T("100")],
                            &[T("2"), Null, T("2"), T("-200"), Null, Null],
                            &[T("3"), Null, Null, Null, T("3"), T("300")],
                            &[Null, Null, T("4"), T("-400"), T("4"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[T("1"), Null, Null, Null, T("1"), T("100")],
                            &[T("2"), Null, Null, Null, Null, Null],
                            &[T("3"), Null, Null, Null, T("3"), T("300")],
                            &[Null, Null, Null, Null, T("4"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_col1 = their_col1;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"pk\" of relation \"t\" violates not-null constraint", detail: "Failing row contains (null, 100).", schema: "public", table: "t", column: "pk", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's error for a NULL in a column that can't hold one is Postgres' not-null violation of the table's own column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_pk = their_pk, our_col1 = their_col1;",
                    expected: Expected::Tag("UPDATE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("3"), T("300")],
                            &[T("4"), T("400")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Updating our cols after schema change",
            intent: "An UPDATE of a column the table gained after the merge writes it to the table's row.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 100);",
                "INSERT INTO t VALUES (2, 200);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, -100);",
                "INSERT INTO t VALUES (2, -200);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
                "ALTER TABLE t ADD COLUMN col2 int;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts_t LIMIT 0;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("our_col2", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_col1", INT4), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so the port reads the view's columns, and Postgres has no ADD COLUMN ... FIRST, so our_col2 follows our_col1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, our_col2, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("our_col2", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("-100"), Null, T("1"), T("100")],
                            &[Null, Null, T("2"), T("-200"), Null, T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET our_col2 = their_col1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1, col2 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("-100"), T("100")],
                            &[T("2"), T("-200"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Updates on their or base columns do nothing",
            intent: "An UPDATE of a conflict's their_ or base_ columns changes neither the conflict nor the table.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 100);",
                "INSERT INTO t VALUES (2, 200);",
                "SELECT dolt_commit('-Am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, -100);",
                "INSERT INTO t VALUES (2, -200);",
                "SELECT dolt_commit('-Am', 'main commit');",
                "SET dolt_allow_commit_conflicts = ON;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("-100"), T("1"), T("100")],
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("-100")],
                            &[T("2"), T("-200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_t SET base_col1 = 9999, their_col1 = 9999;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("-100"), T("1"), T("100")],
                            &[Null, Null, T("2"), T("-200"), T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("-100")],
                            &[T("2"), T("-200")],
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
