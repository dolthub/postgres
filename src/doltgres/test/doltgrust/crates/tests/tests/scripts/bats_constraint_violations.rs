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

//! Dolt's integration-tests/bats/constraint-violations.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_constraint_violations() {
    run_scripts(&[
        ScriptTest {
            name: "constraint-violations: functions blocked with violations",
            intent: "A merge that records violations blocks merging and committing until the violations are deleted.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX v1 ON test (v1);",
                "INSERT INTO test VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-Am', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test VALUES (4, 3), (9, 9);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.test"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("constraint violation")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merging is not possible because you have not committed an active merge", ..E }),
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
                    query: "SELECT dolt_commit('-m', 'this should fail');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: the table(s) test have constraint violations", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_test;",
                    expected: Expected::Tag("DELETE 2"),
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
                    query: "SELECT length(dolt_commit('-m', 'this works')) > 0 AS committed;",
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
                    query: "SELECT * FROM dolt_merge('other');",
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
            name: "constraint-violations: dolt_force_transaction_commit along with dolt_allow_commit_conflicts ignores constraint violations",
            intent: "A merge's violations roll the transaction back unless dolt_force_transaction_commit is on, which also lets a forced commit take them.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX v1 ON test (v1);",
                "INSERT INTO test VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test VALUES (4, 3), (9, 9);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Committing this transaction resulted in a working set with constraint violations, transaction rolled back. This constraint violation may be the result of a previous merge or the result of transaction sequencing. Constraint violations from a merge can be resolved using the dolt_constraint_violations table before committing the transaction. To allow transactions to be committed with constraint violations from a merge or transaction sequencing set dolt_force_transaction_commit to on.", message_contains: true, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-am', 'msg', '--force')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
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
            name: "constraint-violations: ancestor contains fk, main parent remove, other child add, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, other child add, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main child add, other parent remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, other parent remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent add and remove, other child add and remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main parent add and remove, other child add and remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "INSERT INTO parent VALUES (30, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM CHILD WHERE pk = 1;",
                "INSERT INTO child VALUES (2,2), (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent illegal remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main parent illegal remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, other child illegal add, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by other child illegal add, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE pk = 1;",
                "DELETE FROM parent WHERE pk = 10;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "INSERT INTO child VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("3"), T("3"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, other parent illegal remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by other parent illegal remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent remove, other child add, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, other child add, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main child add, other parent remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, other parent remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent add and remove, other child add and remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main parent add and remove, other child add and remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "INSERT INTO parent VALUES (30, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM CHILD WHERE pk = 1;",
                "INSERT INTO child VALUES (2,2), (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent illegal remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main parent illegal remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, other child illegal add, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by other child illegal add, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE pk = 1;",
                "DELETE FROM parent WHERE pk = 10;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "INSERT INTO child VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("3"), T("3"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, other parent illegal remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by other parent illegal remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent remove, other child add, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, other child add, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main child add, other parent remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, other parent remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent add and remove, other child add and remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main parent add and remove, other child add and remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "INSERT INTO parent VALUES (30, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM CHILD WHERE pk = 1;",
                "INSERT INTO child VALUES (2,2), (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent illegal remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main parent illegal remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, other child illegal add, set null",
            intent: "A merge whose sides together leave child rows without their parent, by other child illegal add, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE pk = 1;",
                "DELETE FROM parent WHERE pk = 10;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "INSERT INTO child VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("3"), T("3"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, other parent illegal remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by other parent illegal remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor contains fk, main parent remove with backup, other child add, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove with backup, other child add, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1), (20, 2), (30, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
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
                    query: "SELECT * FROM child ORDER BY pk;",
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
            name: "constraint-violations: ancestor contains fk, main child add, other parent remove with backup, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, other parent remove with backup, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1), (20, 2), (30, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
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
                    query: "SELECT * FROM child ORDER BY pk;",
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
            name: "constraint-violations: ancestor missing fk, main parent remove, other child add, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, other child add, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main child add, other parent remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, other parent remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main parent add and remove, other child add and remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main parent add and remove, other child add and remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "INSERT INTO parent VALUES (30, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "DELETE FROM CHILD WHERE pk = 1;",
                "INSERT INTO child VALUES (2,2), (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main parent illegal remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main parent illegal remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, other child illegal add, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by other child illegal add, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE pk = 1;",
                "DELETE FROM parent WHERE pk = 10;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "SET session_replication_role = replica;",
                "INSERT INTO child VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("3"), T("3"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, other parent illegal remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by other parent illegal remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main parent remove, other child add, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, other child add, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main child add, other parent remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, other parent remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main parent add and remove, other child add and remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main parent add and remove, other child add and remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "INSERT INTO parent VALUES (30, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "DELETE FROM CHILD WHERE pk = 1;",
                "INSERT INTO child VALUES (2,2), (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main parent illegal remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main parent illegal remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, other child illegal add, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by other child illegal add, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE pk = 1;",
                "DELETE FROM parent WHERE pk = 10;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "SET session_replication_role = replica;",
                "INSERT INTO child VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("3"), T("3"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, other parent illegal remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by other parent illegal remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main parent remove, other child add, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, other child add, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL;",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main child add, other parent remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, other parent remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL;",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main parent add and remove, other child add and remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main parent add and remove, other child add and remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "INSERT INTO parent VALUES (30, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL;",
                "DELETE FROM CHILD WHERE pk = 1;",
                "INSERT INTO child VALUES (2,2), (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, main parent illegal remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main parent illegal remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL;",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, other child illegal add, set null",
            intent: "A merge whose sides together leave child rows without their parent, by other child illegal add, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM child WHERE pk = 1;",
                "DELETE FROM parent WHERE pk = 10;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL;",
                "SET session_replication_role = replica;",
                "INSERT INTO child VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("3"), T("3"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing fk, other parent illegal remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by other parent illegal remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL;",
                "SET session_replication_role = replica;",
                "DELETE FROM parent WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                            &[T("foreign key"), T("3"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing parent, main child add, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "INSERT INTO parent VALUES (10, 1);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing parent, main child add, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "INSERT INTO parent VALUES (10, 1);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing parent, main child add, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main child add, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "INSERT INTO parent VALUES (10, 1);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing child, main parent remove, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing child, main parent remove, cascade",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, records each as a foreign key violation declared ON DELETE and ON UPDATE CASCADE, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing child, main parent remove, set null",
            intent: "A merge whose sides together leave child rows without their parent, by main parent remove, records each as a foreign key violation declared ON DELETE and ON UPDATE SET NULL, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 20;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE SET NULL ON UPDATE SET NULL);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"SET NULL\", \"OnUpdate\": \"SET NULL\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: ancestor missing both, other illegal operations, restrict",
            intent: "A merge whose sides together leave child rows without their parent, by other illegal operations, records each as a foreign key violation, with the merged rows.",
            set_up_script: &[
                "CREATE TABLE unrelated (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "INSERT INTO unrelated VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO unrelated VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1));",
                "INSERT INTO parent VALUES (10, 1);",
                "SET session_replication_role = replica;",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"child\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: chained foreign keys, parent deleted on theirs, child added on ours",
            intent: "In a chain of foreign keys, a merge records a violation only where a child row loses its parent.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child1 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_c1 FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "CREATE UNIQUE INDEX child1_v1_idx ON child1 (v1);",
                "CREATE TABLE child2 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_c2 FOREIGN KEY (v1) REFERENCES child1 (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (100, 1), (200, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child1 VALUES (10, 1), (20, 2);",
                "INSERT INTO child2 VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM parent WHERE pk = 200;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("20"), T("2"), T("{\"Index\": \"\", \"Table\": \"child1\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_c1\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("100"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child2 ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: chained foreign keys, parent deleted on ours, child added on theirs",
            intent: "In a chain of foreign keys, a merge records a violation only where a child row loses its parent.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child1 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_c1 FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "CREATE UNIQUE INDEX child1_v1_idx ON child1 (v1);",
                "CREATE TABLE child2 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_c2 FOREIGN KEY (v1) REFERENCES child1 (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (100, 1), (200, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM parent WHERE pk = 200;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child1 VALUES (10, 1), (20, 2);",
                "INSERT INTO child2 VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("20"), T("2"), T("{\"Index\": \"\", \"Table\": \"child1\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_c1\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("100"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child2 ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: chained foreign keys, parent updated on theirs, child added on ours",
            intent: "In a chain of foreign keys, a merge records a violation only where a child row loses its parent.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child1 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_c1 FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "CREATE UNIQUE INDEX child1_v1_idx ON child1 (v1);",
                "CREATE TABLE child2 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_c2 FOREIGN KEY (v1) REFERENCES child1 (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (100, 1), (200, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO child1 VALUES (10, 1), (20, 2);",
                "INSERT INTO child2 VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE parent SET v1 = 3 WHERE pk = 200;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("20"), T("2"), T("{\"Index\": \"\", \"Table\": \"child1\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_c1\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("100"), T("1")],
                            &[T("200"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child2 ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: chained foreign keys, parent updated on ours, child added on theirs",
            intent: "In a chain of foreign keys, a merge records a violation only where a child row loses its parent.",
            set_up_script: &[
                "CREATE TABLE parent (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE TABLE child1 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_c1 FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "CREATE UNIQUE INDEX child1_v1_idx ON child1 (v1);",
                "CREATE TABLE child2 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_c2 FOREIGN KEY (v1) REFERENCES child1 (v1) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO parent VALUES (100, 1), (200, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "UPDATE parent SET v1 = 3 WHERE pk = 200;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO child1 VALUES (10, 1), (20, 2);",
                "INSERT INTO child2 VALUES (1, 1), (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("20"), T("2"), T("{\"Index\": \"\", \"Table\": \"child1\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_c1\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"parent\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_child2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("100"), T("1")],
                            &[T("200"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child2 ORDER BY pk;",
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
            ..S
        },
        ScriptTest {
            name: "constraint-violations: cyclic foreign keys, illegal deletion",
            intent: "In a cycle of foreign keys, a merge of a forced deletion records the violation on each side of the cycle.",
            set_up_script: &[
                "CREATE TABLE t1 (pk BIGINT PRIMARY KEY, v1 BIGINT);",
                "CREATE UNIQUE INDEX v1 ON t1 (v1);",
                "CREATE TABLE t2 (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_t2 FOREIGN KEY (v1) REFERENCES t1 (v1));",
                "CREATE UNIQUE INDEX t2_v1_idx ON t2 (v1);",
                "ALTER TABLE t1 ADD CONSTRAINT fk_t1 FOREIGN KEY (v1) REFERENCES t2 (v1);",
                "SET session_replication_role = replica;",
                "INSERT INTO t1 VALUES (10, 1), (20, 2);",
                "INSERT INTO t2 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('main2');",
                "SELECT dolt_branch('other');",
                "SELECT dolt_branch('other2');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "DELETE FROM t1 WHERE pk = 20;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC1');",
                "SELECT * FROM dolt_checkout('other2');",
                "SET session_replication_role = replica;",
                "DELETE FROM t2 WHERE pk = 2;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'OC2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_t1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_t2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("2"), T("2"), T("{\"Index\": \"\", \"Table\": \"t2\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_t2\", \"ReferencedIndex\": \"v1\", \"ReferencedTable\": \"t1\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY pk;",
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
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
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
                    query: "SELECT * FROM dolt_checkout('main2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other2');",
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
                    query: "SELECT dolt_verify_constraints('--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_t1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("20"), T("2"), T("{\"Index\": \"\", \"Table\": \"t1\", \"Columns\": [\"v1\"], \"OnDelete\": \"NO ACTION\", \"OnUpdate\": \"NO ACTION\", \"ForeignKey\": \"fk_t1\", \"ReferencedIndex\": \"t2_v1_idx\", \"ReferencedTable\": \"t2\", \"ReferencedColumns\": [\"v1\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_t2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("20"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
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
            name: "constraint-violations: self-referential foreign keys, add on ours, delete on theirs",
            intent: "A merge that leaves a self-referencing row without the row it references records a foreign key violation.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES test (pk) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO test VALUES (1, NULL), (2, 1), (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (4, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM test WHERE pk = 3;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("4"), T("3"), T("{\"Index\": \"\", \"Table\": \"test\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"test_pkey\", \"ReferencedTable\": \"test\", \"ReferencedColumns\": [\"pk\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("1")],
                            &[T("4"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: self-referential foreign keys, add on theirs, delete on ours",
            intent: "A merge that leaves a self-referencing row without the row it references records a foreign key violation.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY, v1 BIGINT, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES test (pk) ON DELETE CASCADE ON UPDATE CASCADE);",
                "INSERT INTO test VALUES (1, NULL), (2, 1), (3, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC1');",
                "SELECT dolt_branch('other');",
                "DELETE FROM test WHERE pk = 3;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'MC2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test VALUES (4, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'OC1');",
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
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
                    query: "SELECT dolt_violation_type, pk, v1, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT8), Column("v1", INT8), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("foreign key"), T("4"), T("3"), T("{\"Index\": \"\", \"Table\": \"test\", \"Columns\": [\"v1\"], \"OnDelete\": \"CASCADE\", \"OnUpdate\": \"CASCADE\", \"ForeignKey\": \"fk_name\", \"ReferencedIndex\": \"test_pkey\", \"ReferencedTable\": \"test\", \"ReferencedColumns\": [\"pk\"]}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("1")],
                            &[T("4"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "constraint-violations: unique key violations create unmerged tables",
            intent: "Rows that break a unique index the other side added are violations that leave the table unmerged.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT dolt_branch('right');",
                "CREATE UNIQUE INDEX uniq_col1 ON t (col1);",
                "SELECT dolt_commit('-am', 'add dada');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO t VALUES (1, 1), (2, 1);",
                "SELECT dolt_commit('-am', 'add unique key constraint');",
                "SELECT * FROM dolt_checkout('main');",
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
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
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
            name: "constraint-violations: keyless table constraint violations",
            intent: "A cherry-pick into a keyless table records unique violations with each row's hash, and deleting them after fixing the rows clears them.",
            set_up_script: &[
                "CREATE TABLE aTable (aColumn int NULL, bColumn int NULL); CREATE UNIQUE INDEX aColumn_UNIQUE ON aTable (aColumn ASC); CREATE UNIQUE INDEX bColumn_UNIQUE ON aTable (bColumn ASC);",
                "SELECT dolt_commit('-Am', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'side');",
                "INSERT INTO aTable VALUES (1, 2);",
                "SELECT dolt_commit('-am', 'add side data');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO aTable VALUES (1, 3);",
                "SELECT dolt_commit('-am', 'add main data');",
                "SET dolt_force_transaction_commit = 1;",
                "SELECT * FROM dolt_checkout('side');",
                "SELECT * FROM dolt_cherry_pick(dolt_hashof('main'));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM aTable ORDER BY bColumn;",
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
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
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
                    query: "SELECT dolt_violation_type, upper(encode(dolt_row_hash, 'hex')) AS dolt_row_hash, aColumn, bColumn, dolt_violation_info::text AS dolt_violation_info FROM dolt_constraint_violations_aTable ORDER BY bColumn;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("dolt_row_hash", TEXT), Column("acolumn", INT4), Column("bcolumn", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), Any, T("1"), T("2"), T("{\"Name\": \"acolumn_unique\", \"Columns\": [\"acolumn\"]}")],
                            &[T("unique index"), Any, T("1"), T("3"), T("{\"Name\": \"acolumn_unique\", \"Columns\": [\"acolumn\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE aTable SET aColumn = 2 WHERE bColumn = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_aTable;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) AS count FROM dolt_constraint_violations_aTable;",
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
            name: r#"constraint-violations: altering FKs over PKs does not create bad index"#,
            intent: r#"A foreign key over a primary key added before its parent exists needs no index of its own."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET session_replication_role = replica;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's foreign_key_checks = 0 is Postgres' session_replication_role = replica, and Postgres needs the parent before the reference, so the port makes it first"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE parent (i INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE child (j INT PRIMARY KEY, FOREIGN KEY (j) REFERENCES parent (i));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET session_replication_role = origin;"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM parent WHERE i = 0;"#,
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT indexrelid::regclass::text FROM pg_index WHERE indrelid IN ('parent'::regclass, 'child'::regclass) ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", TEXT)],
                        rows: &[
                            &[T(r#"child_pkey"#)],
                            &[T(r#"parent_pkey"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt index ls is a CLI command, so the port reads pg_index, where Postgres' primary keys have their own indexes"#)),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
