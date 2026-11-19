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

//! Dolt's integration-tests/bats/verify-constraints.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Dolt, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_verify_constraints() {
    run_scripts(&[
        ScriptTest {
            name: "verify-constraints: Constraints verified",
            intent: "Tables whose foreign keys all hold verify cleanly.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child1', 'child2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
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
            name: "verify-constraints: One table fails",
            intent: "Verifying finds the violated foreign key of the one table that has one, and only that one.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "SET session_replication_role = replica;",
                "DELETE FROM parent1 WHERE pk = 1;",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child1');",
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
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("child1_parent1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child2');",
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
                    query: "SELECT dolt_verify_constraints('child1', 'child2');",
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
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("child1_parent1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
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
            name: "verify-constraints: Two tables fail",
            intent: "Verifying finds each table's violated foreign key.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "SET session_replication_role = replica;",
                "DELETE FROM parent2 WHERE pk = 2;",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child1');",
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
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("child1_parent2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child2');",
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
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("child2_parent2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('child1', 'child2');",
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
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("child1_parent2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("child2_parent2")],
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
            name: "verify-constraints: Ignores NULLs",
            intent: "A foreign key with a NULL column holds, and a row without its parent violates it.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
                "CREATE TABLE parent (id bigint PRIMARY KEY, v1 bigint, v2 bigint); CREATE UNIQUE INDEX idx1 ON parent (v1, v2);",
                "CREATE TABLE child (id bigint PRIMARY KEY, v1 bigint, v2 bigint, CONSTRAINT fk_named FOREIGN KEY (v1, v2) REFERENCES parent (v1, v2));",
                "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child VALUES (1, 1, 1), (2, 20, NULL);",
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
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (3, 30, 30);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = origin;",
                    expected: Expected::Tag("SET"),
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
                ScriptTestAssertion {
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("fk_named")],
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
            name: "verify-constraints: CLI missing --all and --output-only, no named tables",
            intent: "Without tables, verifying checks every table's changes since HEAD and records their violations.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
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
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child3 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("fk_name1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child4 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("fk_name2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
            name: "verify-constraints: CLI missing --all and --output-only, named tables",
            intent: "Verifying a named table checks only its changes since HEAD.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
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
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child3 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("fk_name1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child4 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
            name: "verify-constraints: CLI --all",
            intent: "With --all, verifying checks every row of the named tables.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (3, 3);",
                "INSERT INTO child4 VALUES (3, 3);",
                "SET session_replication_role = origin;",
            ],
            assertions: &[
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
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child3 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("fk_name1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT dolt_violation_info->>'ForeignKey' AS fk FROM dolt_constraint_violations_child4 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fk", TEXT)],
                        rows: &[
                            &[T("fk_name2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
            name: "verify-constraints: CLI --output-only",
            intent: "With --output-only, verifying reports violations without recording them.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
            name: "verify-constraints: CLI --all and --output-only",
            intent: "With --all and --output-only, verifying checks every row and records nothing.",
            set_up_script: &[
                "CREATE TABLE parent3 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent3 (v1);",
                "CREATE TABLE child3 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent3 (v1));",
                "CREATE TABLE parent4 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent4 (v1);",
                "CREATE TABLE child4 (pk bigint PRIMARY KEY, v1 bigint, CONSTRAINT fk_name2 FOREIGN KEY (v1) REFERENCES parent4 (v1));",
                "INSERT INTO parent3 VALUES (1, 1);",
                "INSERT INTO parent4 VALUES (2, 2);",
                "SET session_replication_role = replica;",
                "INSERT INTO child3 VALUES (1, 1), (2, 2);",
                "INSERT INTO child4 VALUES (1, 1), (2, 2);",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'has fk violations');",
                "CREATE TABLE parent1 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent1 (v1);",
                "CREATE TABLE parent2 (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent2 (v1);",
                "CREATE TABLE child1 (pk bigint PRIMARY KEY, parent1_v1 bigint, parent2_v1 bigint, CONSTRAINT child1_parent1 FOREIGN KEY (parent1_v1) REFERENCES parent1 (v1), CONSTRAINT child1_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "CREATE TABLE child2 (pk bigint PRIMARY KEY, parent2_v1 bigint, CONSTRAINT child2_parent2 FOREIGN KEY (parent2_v1) REFERENCES parent2 (v1));",
                "INSERT INTO parent1 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO parent2 VALUES (1, 1), (2, 2), (3, 3);",
                "INSERT INTO child1 VALUES (1, 1, 1), (2, 2, 2);",
                "INSERT INTO child2 VALUES (2, 2), (3, 3);",
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
                    query: "SELECT * FROM dolt_constraint_violations ORDER BY \"table\";",
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
