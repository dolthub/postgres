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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_verify_constraints.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_verify_constraints_test_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "verify-constraints: no FK violations",
            intent: "Verifying tables whose foreign keys all hold finds nothing, with or without --all, and records no violations.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
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
                    query: "SELECT dolt_verify_constraints('--all', 'child1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "verify-constraints: FK violations: no named tables",
            intent: "Verifying with no names checks every table's changes since HEAD and records the foreign key violations among them.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child3"), T("1")],
                            &[T("public.child4"), T("1")],
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
            name: "verify-constraints: FK violations: named table",
            intent: "Verifying a named table records only that table's foreign key violations among its changes.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child3');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child3"), T("1")],
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
            name: "verify-constraints: FK violations: named tables",
            intent: "Verifying several named tables records each one's foreign key violations among its changes.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child3', 'child4');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child3"), T("1")],
                            &[T("public.child4"), T("1")],
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
            name: "verify-constraints: FK violations: --all no named tables",
            intent: "Verifying with --all checks every row of every table, including violations HEAD already had.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Ok,
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child3"), T("2")],
                            &[T("public.child4"), T("2")],
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
            name: "verify-constraints: FK violations: --all named table",
            intent: "Verifying a named table with --all checks all its rows, including violations HEAD already had.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all', 'child3');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child3"), T("2")],
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
            name: "verify-constraints: FK violations: --all named tables",
            intent: "Verifying several named tables with --all checks all their rows.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all', 'child3', 'child4');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.child3"), T("2")],
                            &[T("public.child4"), T("2")],
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
            name: "verify-constraints: FK violations: --output-only",
            intent: "Verifying with --output-only reports violations without recording them.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--output-only', 'child3', 'child4');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
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
            name: "verify-constraints: FK violations: --all --output-only",
            intent: "Verifying all rows with --output-only reports violations without recording them.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "SELECT dolt_add('.');",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-afm', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO parent2 VALUES (1,1), (2,2), (3,3);",
                "INSERT INTO child1 VALUES (1,1,1), (2,2,2);",
                "INSERT INTO child2 VALUES (2,2), (3,3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all', '--output-only', 'child3', 'child4');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
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
            name: "verify-constraints: FK violations: bad compound primary key reuse as index - no error",
            intent: "A foreign key on the leading columns of the child's compound key finds no violation when every parent exists.",
            set_up_script: &[
                "CREATE TABLE parent (col1 int NOT NULL, col2 float NOT NULL, PRIMARY KEY (col1, col2));",
                "CREATE TABLE child (col1 int NOT NULL, col2 float NOT NULL, col3 int NOT NULL, col4 float NOT NULL, col5 int NOT NULL, col6 float NOT NULL, PRIMARY KEY (col1, col2, col3, col4, col5, col6), FOREIGN KEY (col1, col2) REFERENCES parent (col1, col2));",
                "SET session_replication_role = replica;",
                "INSERT INTO parent VALUES (1, 2.5), (7, 8.5);",
                "INSERT INTO child VALUES (1, 2.5, 3, 4.5, 5, 6.5), (7, 8.5, 9, 10.5, 11, 12.5);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "verify-constraints: FK violations: bad compound primary key reuse as index - error",
            intent: "A foreign key on the leading columns of the child's compound key finds the row whose parent is missing.",
            set_up_script: &[
                "CREATE TABLE parent (col1 int NOT NULL, col2 float NOT NULL, PRIMARY KEY (col1, col2));",
                "CREATE TABLE child (col1 int NOT NULL, col2 float NOT NULL, col3 int NOT NULL, col4 float NOT NULL, col5 int NOT NULL, col6 float NOT NULL, PRIMARY KEY (col1, col2, col3, col4, col5, col6), FOREIGN KEY (col1, col2) REFERENCES parent (col1, col2));",
                "SET session_replication_role = replica;",
                "INSERT INTO parent VALUES (1, 2.5);",
                "INSERT INTO child VALUES (1, 2.5, 3, 4.5, 5, 6.5), (7, 8.5, 9, 10.5, 11, 12.5);",
                "SET session_replication_role = origin;",
                "SET dolt_force_transaction_commit = 1;",
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
            ],
            ..S
        },
        ScriptTest {
            name: "verify-constraints: FK violations: ignores null",
            intent: "A child row with a NULL in its foreign key columns is no violation, while one with a missing parent is.",
            set_up_script: &[
                "CREATE TABLE parent (id bigint PRIMARY KEY, v1 bigint, v2 bigint); CREATE UNIQUE INDEX ON parent (v1, v2);",
                "CREATE TABLE child (id bigint PRIMARY KEY, v1 bigint, v2 bigint, FOREIGN KEY (v1, v2) REFERENCES parent(v1, v2));",
                "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child VALUES (1, 1, 1), (2, 90, NULL);",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (3, 30, 30);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = origin;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
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
            name: "verify-constraints: unique violations: working set",
            intent: "A merge that duplicates a unique value records unique violations, and verifying the working set finds them again after they are deleted.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, UNIQUE (col1));",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t (pk, col1) VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (pk, col1) VALUES (2, 1);",
                "SELECT dolt_commit('-am', 'insert on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '--squash');",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "verify-constraints: unique violations: --all",
            intent: "Unique violations committed with --force are found again only by verifying with --all.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, UNIQUE (col1));",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t (pk, col1) VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (pk, col1) VALUES (2, 1);",
                "SELECT dolt_commit('-am', 'insert on branch1');",
                "BEGIN;",
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
                    query: "SELECT dolt_commit('-am', 'commiting with conflicts', '--force');",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "verify-constraints: unique violations: working set with named table",
            intent: "Verifying the named table with unique violations in the working set finds them again.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, UNIQUE (col1));",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t (pk, col1) VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (pk, col1) VALUES (2, 1);",
                "SELECT dolt_commit('-am', 'insert on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '--squash');",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('t');",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "verify-constraints: unique violations: working set with named table with no violation",
            intent: "Verifying a named table without violations finds none, though another table has some.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, UNIQUE (col1));",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t (pk, col1) VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (pk, col1) VALUES (2, 1);",
                "SELECT dolt_commit('-am', 'insert on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '--squash');",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('otherTable');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "verify-constraints: unique violations: --all --output-only",
            intent: "Verifying all rows with --output-only reports committed unique violations without recording them.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, UNIQUE (col1));",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t (pk, col1) VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'insert on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t (pk, col1) VALUES (2, 1);",
                "SELECT dolt_commit('-am', 'insert on branch1');",
                "BEGIN;",
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
                    query: "SELECT dolt_commit('-am', 'commiting with conflicts', '--force');",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Name\": \"t_col1_key\", \"Columns\": [\"col1\"]}")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all', '--output-only');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
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
            name: "verify-constraints: check violations: working set",
            intent: "A merge whose rows break a CHECK constraint records check violations, and verifying the working set finds them again after they are deleted.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK (col1 != col2));",
                "INSERT INTO t VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "UPDATE t SET col1 = 42;",
                "SELECT dolt_commit('-am', 'update on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET col2 = 42;",
                "SELECT dolt_commit('-am', 'update on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '--squash');",
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
                            &[T("1"), T("42"), T("42")],
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
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("42"), T("{\"Name\": \"t_check\", \"Expression\": \"(col1 <> col2)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("42"), T("{\"Name\": \"t_check\", \"Expression\": \"(col1 <> col2)\"}")],
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
            name: "verify-constraints: check violations: --all",
            intent: "Check violations committed with --force are found again only by verifying with --all.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK (col1 != col2));",
                "INSERT INTO t VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "UPDATE t SET col1 = 42;",
                "SELECT dolt_commit('-am', 'update on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET col2 = 42;",
                "SELECT dolt_commit('-am', 'update on branch1');",
                "BEGIN;",
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
                    query: "SELECT dolt_commit('-am', 'commiting with conflicts', '--force');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("42"), T("42")],
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
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("42"), T("{\"Name\": \"t_check\", \"Expression\": \"(col1 <> col2)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("42"), T("{\"Name\": \"t_check\", \"Expression\": \"(col1 <> col2)\"}")],
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
            name: "verify-constraints: check violations: working set with named table",
            intent: "Verifying the named table with check violations in the working set finds them again.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK (col1 != col2));",
                "INSERT INTO t VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "UPDATE t SET col1 = 42;",
                "SELECT dolt_commit('-am', 'update on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET col2 = 42;",
                "SELECT dolt_commit('-am', 'update on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '--squash');",
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
                            &[T("1"), T("42"), T("42")],
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
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("42"), T("{\"Name\": \"t_check\", \"Expression\": \"(col1 <> col2)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('t');",
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
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("42"), T("{\"Name\": \"t_check\", \"Expression\": \"(col1 <> col2)\"}")],
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
            name: "verify-constraints: check violations: working set with named table with no violation",
            intent: "Verifying a named table without violations finds none, though another table has some.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK (col1 != col2));",
                "INSERT INTO t VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "UPDATE t SET col1 = 42;",
                "SELECT dolt_commit('-am', 'update on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET col2 = 42;",
                "SELECT dolt_commit('-am', 'update on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '--squash');",
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
                            &[T("1"), T("42"), T("42")],
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
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("42"), T("{\"Name\": \"t_check\", \"Expression\": \"(col1 <> col2)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('otherTable');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "verify-constraints: check violations: --all --output-only",
            intent: "Verifying all rows with --output-only reports committed check violations without recording them.",
            set_up_script: &[
                "CREATE TABLE otherTable (pk int PRIMARY KEY);",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CHECK (col1 != col2));",
                "INSERT INTO t VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('branch1');",
                "UPDATE t SET col1 = 42;",
                "SELECT dolt_commit('-am', 'update on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET col2 = 42;",
                "SELECT dolt_commit('-am', 'update on branch1');",
                "BEGIN;",
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
                    query: "SELECT dolt_commit('-am', 'commiting with conflicts', '--force');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4), Column("col2", INT4)],
                        rows: &[
                            &[T("1"), T("42"), T("42")],
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
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, CAST(dolt_violation_info AS text) AS dolt_violation_info FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("42"), T("{\"Name\": \"t_check\", \"Expression\": \"(col1 <> col2)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("violation_info names the constraint as Postgres does and gives a CHECK expression in pg_get_expr's text; and Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all', '--output-only');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
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
    ]);
}
