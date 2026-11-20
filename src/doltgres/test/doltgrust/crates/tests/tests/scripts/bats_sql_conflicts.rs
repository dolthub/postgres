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

//! Dolt's integration-tests/bats/sql-conflicts.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "sql-conflicts: read from empty table",
            intent: "A table without conflicts reads as an empty conflicts table.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1, pk2));",
                "CREATE TABLE empty_table (pk bigint NOT NULL, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create tables');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts_empty_table;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT8), Column("our_pk", INT8), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT8), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
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
            name: "sql-conflicts: add conflict",
            intent: "Rows both sides added with the same key are conflicts that deleting them resolves, and deleting a resolved conflict again changes nothing.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1, pk2));",
                "CREATE TABLE empty_table (pk bigint NOT NULL, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create tables');",
                "SELECT dolt_branch('feature_branch', 'main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "INSERT INTO two_pk (pk1, pk2, c1, c2) VALUES (0, 0, 0, 0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature_branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "INSERT INTO two_pk (pk1, pk2, c1, c2) VALUES (0, 0, 1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changed feature_branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature_branch', '-m', 'merge');",
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
                    query: "SELECT * FROM dolt_conflicts ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.one_pk"), T("1")],
                            &[T("public.two_pk"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk1, our_pk1, their_pk1, base_c1, our_c1, their_c1, base_c2, our_c2, their_c2 FROM dolt_conflicts_one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk1", INT8), Column("our_pk1", INT8), Column("their_pk1", INT8), Column("base_c1", INT8), Column("our_c1", INT8), Column("their_c1", INT8), Column("base_c2", INT8), Column("our_c2", INT8), Column("their_c2", INT8)],
                        rows: &[
                            &[Null, T("0"), T("0"), Null, T("0"), T("1"), Null, T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk1, our_pk1, their_pk1, base_pk2, our_pk2, their_pk2, base_c1, our_c1, their_c1, base_c2, our_c2, their_c2 FROM dolt_conflicts_two_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk1", INT8), Column("our_pk1", INT8), Column("their_pk1", INT8), Column("base_pk2", INT8), Column("our_pk2", INT8), Column("their_pk2", INT8), Column("base_c1", INT8), Column("our_c1", INT8), Column("their_c1", INT8), Column("base_c2", INT8), Column("our_c2", INT8), Column("their_c2", INT8)],
                        rows: &[
                            &[Null, T("0"), T("0"), Null, T("0"), T("0"), Null, T("0"), T("1"), Null, T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_our_diff_type, dolt_their_diff_type FROM dolt_conflicts_one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_our_diff_type", TEXT), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("added"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_our_diff_type, dolt_their_diff_type FROM dolt_conflicts_two_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_our_diff_type", TEXT), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("added"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_two_pk WHERE our_pk1 = 0 AND our_pk2 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_one_pk WHERE our_pk1 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
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
                    query: "DELETE FROM dolt_conflicts_one_pk WHERE our_pk1 = 0;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts: modify conflict",
            intent: "Rows both sides changed differently are conflicts that deleting them resolves.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1, pk2));",
                "CREATE TABLE empty_table (pk bigint NOT NULL, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create tables');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "INSERT INTO two_pk (pk1, pk2, c1, c2) VALUES (0, 0, 0, 0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial values');",
                "SELECT dolt_branch('feature_branch', 'main');",
                "UPDATE one_pk SET c1 = 1, c2 = 1 WHERE pk1 = 0;",
                "UPDATE two_pk SET c1 = 1, c2 = 1 WHERE pk1 = 0 AND pk2 = 0;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature_branch');",
                "UPDATE one_pk SET c1 = 2, c2 = 2 WHERE pk1 = 0;",
                "UPDATE two_pk SET c1 = 2, c2 = 2 WHERE pk1 = 0 AND pk2 = 0;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changed feature_branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature_branch', '-m', 'merge');",
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
                    query: "SELECT * FROM dolt_conflicts ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.one_pk"), T("1")],
                            &[T("public.two_pk"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk1, our_pk1, their_pk1, base_c1, our_c1, their_c1, base_c2, our_c2, their_c2 FROM dolt_conflicts_one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk1", INT8), Column("our_pk1", INT8), Column("their_pk1", INT8), Column("base_c1", INT8), Column("our_c1", INT8), Column("their_c1", INT8), Column("base_c2", INT8), Column("our_c2", INT8), Column("their_c2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("1"), T("2"), T("0"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk1, our_pk1, their_pk1, base_pk2, our_pk2, their_pk2, base_c1, our_c1, their_c1, base_c2, our_c2, their_c2 FROM dolt_conflicts_two_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk1", INT8), Column("our_pk1", INT8), Column("their_pk1", INT8), Column("base_pk2", INT8), Column("our_pk2", INT8), Column("their_pk2", INT8), Column("base_c1", INT8), Column("our_c1", INT8), Column("their_c1", INT8), Column("base_c2", INT8), Column("our_c2", INT8), Column("their_c2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("1"), T("2"), T("0"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_our_diff_type, dolt_their_diff_type FROM dolt_conflicts_one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_our_diff_type", TEXT), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("modified"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_our_diff_type, dolt_their_diff_type FROM dolt_conflicts_two_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_our_diff_type", TEXT), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("modified"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_one_pk WHERE our_pk1 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_two_pk WHERE our_pk1 = 0 AND our_pk2 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
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
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts: delete modify conflict",
            intent: "A row one side changed and the other deleted is a conflict that deleting it resolves.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1, pk2));",
                "CREATE TABLE empty_table (pk bigint NOT NULL, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create tables');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "INSERT INTO two_pk (pk1, pk2, c1, c2) VALUES (0, 0, 0, 0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial values');",
                "SELECT dolt_branch('feature_branch', 'main');",
                "UPDATE one_pk SET c1 = 1, c2 = 1 WHERE pk1 = 0;",
                "UPDATE two_pk SET c1 = 1, c2 = 1 WHERE pk1 = 0 AND pk2 = 0;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature_branch');",
                "DELETE FROM one_pk WHERE pk1 = 0;",
                "DELETE FROM two_pk WHERE pk1 = 0 AND pk2 = 0;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changed feature_branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature_branch', '-m', 'merge');",
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
                    query: "SELECT * FROM dolt_conflicts ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.one_pk"), T("1")],
                            &[T("public.two_pk"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk1, our_pk1, their_pk1, base_c1, our_c1, their_c1, base_c2, our_c2, their_c2 FROM dolt_conflicts_one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk1", INT8), Column("our_pk1", INT8), Column("their_pk1", INT8), Column("base_c1", INT8), Column("our_c1", INT8), Column("their_c1", INT8), Column("base_c2", INT8), Column("our_c2", INT8), Column("their_c2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), Null, T("0"), T("1"), Null, T("0"), T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk1, our_pk1, their_pk1, base_pk2, our_pk2, their_pk2, base_c1, our_c1, their_c1, base_c2, our_c2, their_c2 FROM dolt_conflicts_two_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk1", INT8), Column("our_pk1", INT8), Column("their_pk1", INT8), Column("base_pk2", INT8), Column("our_pk2", INT8), Column("their_pk2", INT8), Column("base_c1", INT8), Column("our_c1", INT8), Column("their_c1", INT8), Column("base_c2", INT8), Column("our_c2", INT8), Column("their_c2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), Null, T("0"), T("0"), Null, T("0"), T("1"), Null, T("0"), T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_our_diff_type, dolt_their_diff_type FROM dolt_conflicts_one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_our_diff_type", TEXT), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("modified"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_our_diff_type, dolt_their_diff_type FROM dolt_conflicts_two_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_our_diff_type", TEXT), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("modified"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_one_pk WHERE our_pk1 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_two_pk WHERE our_pk1 = 0 AND our_pk2 = 0;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
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
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts: multiple conflicts",
            intent: "Every row the sides changed differently counts as one conflict of the table.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1, pk2));",
                "CREATE TABLE empty_table (pk bigint NOT NULL, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create tables');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial values');",
                "SELECT dolt_branch('feature_branch', 'main');",
                "UPDATE one_pk SET c1 = 1, c2 = 1;",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (4, 1, 1);",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (5, 3, 3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature_branch');",
                "UPDATE one_pk SET c1 = 2, c2 = 2;",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (4, 2, 2);",
                "DELETE FROM one_pk WHERE pk1 = 3;",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (5, 3, 3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changed feature_branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature_branch', '-m', 'merge');",
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
                            &[T("public.one_pk"), T("5")],
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
