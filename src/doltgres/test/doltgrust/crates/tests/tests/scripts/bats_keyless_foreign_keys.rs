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

//! Dolt's integration-tests/bats/keyless-foreign-keys.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_keyless_foreign_keys() {
    run_scripts(&[
        ScriptTest {
            name: "keyless-foreign-keys: Merge valid onto parent",
            intent: "A forced parent update on one side merges with the other side's new parent row, the cascade keeping the children.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                "INSERT INTO child VALUES (2, 1, 1), (3, 2, 2), (4, 3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO parent VALUES (4, 3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added row');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "UPDATE parent SET v1 = v1 - 1;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'updated parent');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, v1 FROM parent ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v1 FROM child ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a foreign key that references columns without a unique constraint is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Merge invalid onto parent",
            intent: "A forced parent update that leaves a child without its parent records a violation after the merge.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                "INSERT INTO child VALUES (2, 1, 1), (3, 2, 2), (4, 3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO parent VALUES (4, 4, 4);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added row');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "UPDATE parent SET v1 = v1 - 1;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'updated parent');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
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
            ],
            skip: Some("UPDATE parent SET v1 = v1 - 1 passes the unique index in Postgres only because its heap order moves v1 = 1 before v1 = 2, while Doltgrust orders a table without a primary key by its rows' hashes"),
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Merge valid onto child",
            intent: "A forced child update on one side merges with the other side's new parent row that it now references.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                "INSERT INTO child VALUES (2, 1, 1), (3, 2, 2), (4, 3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO parent VALUES (4, 4, 4);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added row');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "UPDATE child SET v1 = v1 + 1;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'updated child');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, v1 FROM parent ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v1 FROM child ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Merge invalid onto child",
            intent: "A forced child update that references a missing parent records a violation after the merge.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                "INSERT INTO child VALUES (2, 1, 1), (3, 2, 2), (4, 3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO parent VALUES (4, 4, 4);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added row');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "UPDATE child SET v1 = v1 - 1;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'updated child');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
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
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Merge valid onto parent and child",
            intent: "Forced parent and child updates on one side merge with the other side's new parent rows.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                "INSERT INTO child VALUES (2, 1, 1), (3, 2, 2), (4, 3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO parent VALUES (4, 3, 3), (5, 4, 4);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added row');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "UPDATE parent SET v1 = v1 - 1;",
                "UPDATE child SET v1 = v1 + 1;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'updated both');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, v1 FROM parent ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), T("3")],
                            &[T("5"), T("4")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v1 FROM child ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a foreign key that references columns without a unique constraint is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Merge invalid onto parent and child",
            intent: "Forced parent and child updates that leave a child without its parent record a violation after the merge.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                "INSERT INTO child VALUES (2, 1, 1), (3, 2, 2), (4, 3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO parent VALUES (4, 3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added row');",
                "SELECT * FROM dolt_checkout('other');",
                "SET session_replication_role = replica;",
                "UPDATE parent SET v1 = v1 - 1;",
                "UPDATE child SET v1 = v1 + 1;",
                "SET session_replication_role = origin;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('--force', '-m', 'updated both');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
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
            ],
            skip: Some("MySQL-only: a foreign key that references columns without a unique constraint is MySQL's"),
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Resolve catches violations",
            intent: "Resolving conflicts on keyless tables refuses while foreign key violations remain.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
                "ALTER TABLE child ADD CONSTRAINT fk_v1 FOREIGN KEY (v1) REFERENCES parent (v1);",
                "INSERT INTO parent VALUES (0, 0, 0);",
                "INSERT INTO child VALUES (0, 0, 0);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "INSERT INTO parent VALUES (1, 1, 1);",
                "INSERT INTO child VALUES (1, 1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added 1s');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO parent VALUES (1, 2, 2);",
                "INSERT INTO child VALUES (1, 2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added 2s');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
                "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'parent');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "violation", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'child');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "violation", message_contains: true, ..E }),
                    ..A
                },
            ],
            skip: Some("Dolt skips its own bats test (keyless fk todo), and Dolt's dolt_conflicts_resolve succeeds here as Doltgrust's does"),
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: FKs move with the working set on checkout",
            intent: "An uncommitted foreign key stays with its branch's working set across checkouts.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added parent and child tables');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE child ADD CONSTRAINT fk_v1 FOREIGN KEY (v1) REFERENCES parent (v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt checkout on the CLI moves the working set to the new branch and refuses when it overlaps the branch's changes, where dolt_checkout leaves the working set on the branch it leaves, as Dolt's SQL dolt_checkout does; Dolt skips this test (\"foreign keys don't travel with the working set when checking out a new branch\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname::text FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname::text FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT)],
                        rows: &[
                            &[T("fk_v1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: test foreign-key on commit checks",
            intent: "Changes made with foreign key checks off that leave the keys valid commit.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX id ON colors (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX color_index ON colors (color);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), FOREIGN KEY (color) REFERENCES colors(color));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on colors's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON objects (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'initialize');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM colors WHERE id = 3 OR id = 4;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors (id,color) VALUES (5,'blue');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE colors SET color='orange' WHERE color = 'green';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE objects SET color='orange' WHERE color = 'green';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,color) VALUES (4,'car','red'),(5,'dress','orange');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name) VALUES (6,'glass slipper');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM colors;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("2"), T("orange")],
                            &[T("1"), T("red")],
                            &[T("5"), T("blue")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM objects;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("color", VARCHAR)],
                        rows: &[
                            &[T("3"), T("shoe"), T("blue")],
                            &[T("5"), T("dress"), T("orange")],
                            &[T("6"), T("glass slipper"), Null],
                            &[T("2"), T("ball"), T("orange")],
                            &[T("4"), T("car"), T("red")],
                            &[T("1"), T("truck"), T("red")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'update 1');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: test multi-field foreign-key on commit checks",
            intent: "Changes made with foreign key checks off that leave two-column keys valid commit.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON colors (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX color_index ON colors (color);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE materials (id int NOT NULL, material VARCHAR(32) NOT NULL, color VARCHAR(32), FOREIGN KEY (color) REFERENCES colors(color));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on colors's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX materials_idx ON materials (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX color_mat_index ON materials (color, material);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), material VARCHAR(32), FOREIGN KEY (color,material) REFERENCES materials(color,material));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX objects_idx ON objects (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple'),(10,'brown');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO materials (id,material,color) VALUES (1,'steel','red'),(2,'rubber','green'),(3,'leather','blue'),(10,'dirt','brown'),(11,'air',NULL);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,color,material) VALUES (1,'truck','red','steel'),(2,'ball','green','rubber'),(3,'shoe','blue','leather'),(11,'tornado',NULL,'air');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'initialize');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM colors WHERE id = 3 OR id = 4;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors (id,color) VALUES (5,'blue');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM materials WHERE id IN (1,10);",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO materials (id,material,color) VALUES (4,'steel','red'),(5,'fiber glass','red'),(6,'cotton','orange');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE colors SET color='orange' WHERE color = 'green';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE materials SET color='orange' WHERE color = 'green';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE objects SET color='orange' WHERE color = 'green';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,color,material) VALUES (4,'car','red','fiber glass'),(5,'dress','orange','cotton');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO materials (id,material) VALUES (7,'glass');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,material) VALUES (6,'glass slipper','glass');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM objects WHERE material = 'air';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM materials WHERE material = 'air';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM colors;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("2"), T("orange")],
                            &[T("1"), T("red")],
                            &[T("5"), T("blue")],
                            &[T("10"), T("brown")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM materials;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("material", VARCHAR), Column("color", VARCHAR)],
                        rows: &[
                            &[T("7"), T("glass"), Null],
                            &[T("2"), T("rubber"), T("orange")],
                            &[T("5"), T("fiber glass"), T("red")],
                            &[T("4"), T("steel"), T("red")],
                            &[T("6"), T("cotton"), T("orange")],
                            &[T("3"), T("leather"), T("blue")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM objects;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("color", VARCHAR), Column("material", VARCHAR)],
                        rows: &[
                            &[T("1"), T("truck"), T("red"), T("steel")],
                            &[T("4"), T("car"), T("red"), T("fiber glass")],
                            &[T("2"), T("ball"), T("orange"), T("rubber")],
                            &[T("3"), T("shoe"), T("blue"), T("leather")],
                            &[T("5"), T("dress"), T("orange"), T("cotton")],
                            &[T("6"), T("glass slipper"), Null, T("glass")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'update 1');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: ALTER TABLE Single Named FOREIGN KEY",
            intent: "A named foreign key added with ALTER TABLE shows in the table's definition.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_named"), T("FOREIGN KEY (v1) REFERENCES parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: CREATE TABLE Single Named FOREIGN KEY",
            intent: "A named foreign key declared in CREATE TABLE shows in the table's definition.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE sibling (id int, v1 int, CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES parent(v1));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON sibling (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'sibling'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_named"), T("FOREIGN KEY (v1) REFERENCES parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: parent table index required",
            intent: "A foreign key needs an index on the parent's referenced columns.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (v1,v2) REFERENCES parent(v1,v2);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "there is no unique constraint matching given keys for referenced table \"parent\"", ..E }),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_id FOREIGN KEY (v1) REFERENCES parent(id);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "there is no unique constraint matching given keys for referenced table \"parent\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: CREATE TABLE Name Collision",
            intent: "CREATE TABLE fails when its name or a foreign key's name is taken.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE child (id int, v1 int, CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1), CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1));",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"child\" already exists", ..E }),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON child (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: CREATE TABLE Type Mismatch",
            intent: "A foreign key between columns of incompatible types is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE sibling (pk int PRIMARY KEY, v1 text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sibling ADD CONSTRAINT fk1 FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "foreign key constraint \"fk1\" cannot be implemented", detail: "Key columns \"v1\" of the referencing table and \"v1\" of the referenced table are of incompatible types: text and integer.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: CREATE TABLE Key Count Mismatch",
            intent: "A foreign key whose column counts differ is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (v1) REFERENCES parent(v1,v2);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "there is no unique constraint matching given keys for referenced table \"parent\"", ..E }),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (v1,v2) REFERENCES parent(v1);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "number of referencing and referenced columns for foreign key disagree", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: CREATE TABLE Non-existent Table",
            intent: "A foreign key referencing a missing table is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (v1) REFERENCES father(v1);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"father\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on father's referenced columns is unique")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: CREATE TABLE Non-existent Columns",
            intent: "A foreign key naming a missing column on either side is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (random) REFERENCES parent(v1);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"random\" referenced in foreign key constraint does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (v1) REFERENCES parent(random);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"random\" referenced in foreign key constraint does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: ALTER TABLE Foreign Key Name Collision",
            intent: "A second foreign key with a taken name is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "constraint \"fk_name\" for relation \"child\" already exists", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: ALTER TABLE DROP FOREIGN KEY",
            intent: "Dropping a foreign key removes it from the definition and keeps the indexes, and dropping it again fails.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'parent' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("v1"), T("CREATE UNIQUE INDEX v1 ON public.parent USING btree (v1)")],
                            &[T("v2"), T("CREATE UNIQUE INDEX v2 ON public.parent USING btree (v2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'child' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes, and Postgres makes no index for a foreign key's referencing columns, where MySQL does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk_name;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'parent' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("v1"), T("CREATE UNIQUE INDEX v1 ON public.parent USING btree (v1)")],
                            &[T("v2"), T("CREATE UNIQUE INDEX v2 ON public.parent USING btree (v2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'child' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes, and Postgres makes no index for a foreign key's referencing columns, where MySQL does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk_name;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "constraint \"fk_name\" of relation \"child\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: ADD FOREIGN KEY fails on existing table when data would cause violation",
            intent: "Adding a foreign key that existing rows violate fails.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child  VALUES (1, 1, 1), (2, 3, 2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"fk_name\"", detail: "Key (v1)=(3) is not present in table \"parent\".", schema: "public", table: "child", constraint: "fk_name", ..E }),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: RENAME TABLE",
            intent: "Renaming the parent and the child keeps the foreign key pointing at the renamed parent.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent RENAME TO new_parent;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES new_parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child RENAME TO new_child;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'new_child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES new_parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: dolt table mv",
            intent: "Moving the parent and the child keeps the foreign key pointing at the moved parent.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent RENAME TO new_parent;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("dolt table mv has no SQL form, so ALTER TABLE ... RENAME TO moves the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES new_parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child RENAME TO new_child;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("dolt table mv has no SQL form, so ALTER TABLE ... RENAME TO moves the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'new_child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES new_parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: DROP TABLE",
            intent: "A parent can't be dropped while a child references it, and can after the child is dropped.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'parent' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("v1"), T("CREATE UNIQUE INDEX v1 ON public.parent USING btree (v1)")],
                            &[T("v2"), T("CREATE UNIQUE INDEX v2 ON public.parent USING btree (v2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'parent' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("v1"), T("CREATE UNIQUE INDEX v1 ON public.parent USING btree (v1)")],
                            &[T("v2"), T("CREATE UNIQUE INDEX v2 ON public.parent USING btree (v2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'child' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes, and Postgres makes no index for a foreign key's referencing columns, where MySQL does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE parent;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table parent because other objects depend on it", detail: "constraint fk_name on table child depends on table parent", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE child;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'parent' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("v1"), T("CREATE UNIQUE INDEX v1 ON public.parent USING btree (v1)")],
                            &[T("v2"), T("CREATE UNIQUE INDEX v2 ON public.parent USING btree (v2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE parent;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: dolt table rm",
            intent: "Removing a referenced parent fails until its child is removed.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'parent' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("v1"), T("CREATE UNIQUE INDEX v1 ON public.parent USING btree (v1)")],
                            &[T("v2"), T("CREATE UNIQUE INDEX v2 ON public.parent USING btree (v2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'child' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes, and Postgres makes no index for a foreign key's referencing columns, where MySQL does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE parent;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table parent because other objects depend on it", detail: "constraint fk_name on table child depends on table parent", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    dolt: Some(Dolt::Differs("dolt table rm has no SQL form, so DROP TABLE removes the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE child;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::Differs("dolt table rm has no SQL form, so DROP TABLE removes the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'parent' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("v1"), T("CREATE UNIQUE INDEX v1 ON public.parent USING btree (v1)")],
                            &[T("v2"), T("CREATE UNIQUE INDEX v2 ON public.parent USING btree (v2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE parent;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::Differs("dolt table rm has no SQL form, so DROP TABLE removes the table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: indexes used by foreign keys can't be dropped",
            intent: "An index a foreign key needs can't be dropped until the key is.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX child_v1 ON child (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX child_v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    dolt: Some(Dolt::Differs("Postgres needs no index on a foreign key's referencing columns, so the child's index drops while the key exists, where MySQL keeps it for the key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX v1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop index v1 because other objects depend on it", detail: "constraint fk_name on table child depends on index v1", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk_name;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX child_v1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "index \"child_v1\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres needs no index on a foreign key's referencing columns, so the child's index drops while the key exists, where MySQL keeps it for the key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: dolt table cp",
            intent: "A copy of a child table has no foreign key, and the original keeps its own.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE one (id bigint, extra bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON one (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX extra ON one (extra);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE two (id bigint, one_extra bigint, FOREIGN KEY (one_extra) REFERENCES one(extra));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on one's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX two_idx ON two (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE two_new AS SELECT * FROM two;",
                    expected: Expected::Tag("SELECT 0"),
                    dolt: Some(Dolt::Differs("dolt table cp has no SQL form, so CREATE TABLE ... AS copies the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'two_new'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'two'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("two_one_extra_fkey"), T("FOREIGN KEY (one_extra) REFERENCES one(extra)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: ALTER TABLE RENAME COLUMN",
            intent: "Renaming the referenced and referencing columns keeps the foreign key on them.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent RENAME COLUMN v1 TO v1_new;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child RENAME COLUMN v1 TO v1_new;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk1"), T("FOREIGN KEY (v1_new) REFERENCES parent(v1_new)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: DROP COLUMN",
            intent: "A column a foreign key uses can't be dropped until the key is.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'initial commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent DROP COLUMN v1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop column v1 of table parent because other objects depend on it", detail: "constraint fk_name on table child depends on column v1 of table parent", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk_name;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent DROP COLUMN v1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP COLUMN v1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk_name;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "constraint \"fk_name\" of relation \"child\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP COLUMN v1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v1\" of relation \"child\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Disallow change column type when SET NULL",
            intent: "Changing the type of a column a SET NULL key uses fails.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE SET NULL ON UPDATE SET NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ALTER COLUMN parent_extra TYPE bigint, ALTER COLUMN parent_extra DROP NOT NULL, ALTER COLUMN parent_extra DROP IDENTITY IF EXISTS, ALTER COLUMN parent_extra DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"parent_extra\" of relation \"child\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ALTER COLUMN parent_extra TYPE bigint, ALTER COLUMN parent_extra DROP NOT NULL, ALTER COLUMN parent_extra DROP IDENTITY IF EXISTS, ALTER COLUMN parent_extra DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"parent_extra\" of relation \"child\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: SQL RESTRICT",
            intent: "RESTRICT refuses updates and deletes of referenced parent rows.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE one (pk bigint, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON one (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX one_v1 ON one (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE two (pk bigint, v1 bigint, v2 bigint, CONSTRAINT fk_name_1 FOREIGN KEY (v1) REFERENCES one(v1) ON DELETE RESTRICT ON UPDATE RESTRICT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on one's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX two_idx ON two (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO one VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO two VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'two'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name_1"), T("FOREIGN KEY (v1) REFERENCES one(v1) ON UPDATE RESTRICT ON DELETE RESTRICT")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one SET v1 = v1 + v2;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"one_v1\"", detail: "Key (v1)=(2) already exists.", schema: "public", table: "one", constraint: "one_v1", ..E }),
                    skip: Some("keyless tables scan in content-hash order, where Postgres scans its heap in insertion order, so this UPDATE changes row 2 before row 1 and breaks only the foreign key (Stage 2 item 7263)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one SET v1 = v1;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one;",
                    expected: Expected::Error(Diagnostic { code: "23001", message: "update or delete on table \"one\" violates RESTRICT setting of foreign key constraint \"fk_name_1\" on table \"two\"", detail: "is referenced from table \"two\".", detail_contains: true, schema: "public", table: "two", constraint: "fk_name_1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: SQL no reference options",
            intent: "A foreign key without actions refuses updates and deletes of referenced parent rows.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE one (pk bigint, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON one (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX one_v1 ON one (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE two (pk bigint, v1 bigint, v2 bigint, CONSTRAINT fk_name_1 FOREIGN KEY (v1) REFERENCES one(v1));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on one's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX two_idx ON two (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO one VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO two VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'two'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name_1"), T("FOREIGN KEY (v1) REFERENCES one(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one SET v1 = v1 + v2;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"one_v1\"", detail: "Key (v1)=(2) already exists.", schema: "public", table: "one", constraint: "one_v1", ..E }),
                    skip: Some("keyless tables scan in content-hash order, where Postgres scans its heap in insertion order, so this UPDATE changes row 2 before row 1 and breaks only the foreign key (Stage 2 item 7263)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one SET v1 = v1;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "update or delete on table \"one\" violates foreign key constraint \"fk_name_1\" on table \"two\"", detail: "Key (v1)=(2) is still referenced from table \"two\".", schema: "public", table: "two", constraint: "fk_name_1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: SQL INSERT multiple keys violates only one",
            intent: "An insert that breaks one of two foreign keys names that key, and NULLs satisfy both.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE one (pk bigint, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON one (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX one_v1 ON one (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX one_v2 ON one (v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE two (pk bigint, v1 bigint, v2 bigint, CONSTRAINT fk_name_1 FOREIGN KEY (v1) REFERENCES one(v1), CONSTRAINT fk_name_2 FOREIGN KEY (v2) REFERENCES one(v2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on one's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX two_idx ON two (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO one VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO two VALUES (1, NULL, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO two VALUES (2, NULL, 4);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"two\" violates foreign key constraint \"fk_name_2\"", detail: "Key (v2)=(4) is not present in table \"one\".", schema: "public", table: "two", constraint: "fk_name_2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO two VALUES (3, 4, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"two\" violates foreign key constraint \"fk_name_1\"", detail: "Key (v1)=(4) is not present in table \"one\".", schema: "public", table: "two", constraint: "fk_name_1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO two VALUES (4, NULL, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: dolt table import",
            intent: "Updating parent rows through an import cascades to their children.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO parent VALUES (1, 1, 1), (2, 2, 2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child  VALUES (1, 1, 1), (2, 2, 2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM parent;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Commit all",
            intent: "Foreign keys survive commits, checkouts, and a parent's rename on another branch.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE CASCADE ON UPDATE RESTRICT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE RESTRICT ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'has fk');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE RESTRICT ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'still_has_fk');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE child;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::Differs("dolt table rm has no SQL form, so DROP TABLE removes the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'removed child');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('still_has_fk');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE RESTRICT ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent RENAME TO super_parent;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES super_parent(v1) ON UPDATE RESTRICT ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'renamed parent');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES super_parent(v1) ON UPDATE RESTRICT ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'last_commit', 'HEAD~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE RESTRICT ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Commit then rename parent, child, and columns",
            intent: "A foreign key renamed with its tables and columns commits, and the original branch keeps the old names.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'has fk');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE CASCADE ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'original');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent RENAME TO new_parent;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child RENAME TO new_child;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE new_parent RENAME COLUMN v1 TO vnew;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE new_child RENAME COLUMN v1 TO vnew;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'new_child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (vnew) REFERENCES new_parent(vnew) ON UPDATE CASCADE ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'renamed everything');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'new_child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (vnew) REFERENCES new_parent(vnew) ON UPDATE CASCADE ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('original');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE CASCADE ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Commit then recreate key with different columns",
            intent: "A foreign key recreated on other columns under the same name commits, and the original branch keeps the old one.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'has fk');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE CASCADE ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'original');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX v1v2 ON parent (v1,v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk_name;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1,v2) REFERENCES parent(v1,v2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1, v2) REFERENCES parent(v1, v2)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'different fk same name');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1, v2) REFERENCES parent(v1, v2)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('original');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE CASCADE ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Commit --force",
            intent: "A commit of a child whose parent isn't staged fails unless forced.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('child');",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { code: "00000", message: "dolt_add also stages public.parent", ..N }],
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'will fail');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("dolt_add stages a new foreign key's referenced table with the referencing one (§IV.3a), so the commit takes the parent too and nothing is left to force")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--force', '-m', 'will succeed');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    dolt: Some(Dolt::Differs("dolt_add stages a new foreign key's referenced table with the referencing one (§IV.3a), so the commit takes the parent too and nothing is left to force")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'last_commit', 'HEAD~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'nothing changed, will fail');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('parent');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: the table(s) parent do not exist", ..E }),
                    dolt: Some(Dolt::Differs("dolt_add stages a new foreign key's referenced table with the referencing one (§IV.3a), so the commit takes the parent too and nothing is left to force")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'parent commits just fine without child, child not in working set anymore');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    dolt: Some(Dolt::Differs("dolt_add stages a new foreign key's referenced table with the referencing one (§IV.3a), so the commit takes the parent too and nothing is left to force")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Commit then delete foreign key",
            intent: "A dropped foreign key commits, and the original branch keeps it.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'has fk');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE CASCADE ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'original');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk_name;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'no foreign key');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('original');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_name"), T("FOREIGN KEY (v1) REFERENCES parent(v1) ON UPDATE CASCADE ON DELETE CASCADE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Reset staged table",
            intent: "A staged parent and child unstage with dolt_reset.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('parent');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: Commit, rename parent, commit only child",
            intent: "A child whose renamed parent isn't staged can't commit until the parent is.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent(v1) ON DELETE CASCADE ON UPDATE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'has fk');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent RENAME TO super_parent;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('child');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'will fail since super_parent is missing');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    dolt: Some(Dolt::Differs("A foreign key names its table by identity (§IV.3a), so renaming the parent changes only the parent, and the child has nothing to commit")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('super_parent');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'passes now');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: extended names supported",
            intent: "Foreign key names with characters like $ and -, quoted, are kept.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE parent2 (pk bigint, v1 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx_v1 ON parent2 (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx_pk ON parent2 (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE child2 (pk bigint, v1 bigint, CONSTRAINT circuits_123abc4d_fk_circuits_ FOREIGN KEY (pk) REFERENCES parent2 (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX child2_idx_v1 ON child2 (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX child2_idx_pk ON child2 (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child2 ADD CONSTRAINT \"$not-possible-before_\" FOREIGN KEY (v1) REFERENCES parent2 (v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on parent2's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child2'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("$not-possible-before_"), T("FOREIGN KEY (v1) REFERENCES parent2(v1)")],
                            &[T("circuits_123abc4d_fk_circuits_"), T("FOREIGN KEY (pk) REFERENCES parent2(pk)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: self-referential same column(s)",
            intent: "A foreign key referencing its own columns is accepted.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX v1v2 ON parent(v1, v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent ADD CONSTRAINT fk_name1 FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent ADD CONSTRAINT fk_name2 FOREIGN KEY (v1, v2) REFERENCES parent(v1, v2);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "there is no unique constraint matching given keys for referenced table \"parent\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: self referential foreign keys do not break committing",
            intent: "A table with a foreign key to itself commits.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (id char(32) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"keyless fk todo\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx ON test (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD COLUMN new_col char(32) NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD CONSTRAINT fk_test FOREIGN KEY (new_col) REFERENCES test(id);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on test's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'committed');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: deleting and reading",
            intent: "A child dropped and recreated with the same foreign key commits.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE parent2 (pk bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx ON parent2 (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE child2 (pk bigint, CONSTRAINT child2_fk FOREIGN KEY (pk) REFERENCES parent2 (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the parent's v1 and v2 indexes are unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX child2_idx ON child2 (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'parent2 and child2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE child2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'drop child');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE child2 (pk bigint, CONSTRAINT child2_fk FOREIGN KEY (pk) REFERENCES parent2 (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on parent2's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX child2_idx ON child2 (pk);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'new child');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child2'::regclass AND contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("child2_fk"), T("FOREIGN KEY (pk) REFERENCES parent2(pk)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_constraint lists the table's foreign keys")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless-foreign-keys: child violation correctly detected",
            intent: "Deleting a referenced parent row fails and keeps the row.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON colors (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX color_index ON colors (color);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), CONSTRAINT color_fk FOREIGN KEY (color) REFERENCES colors(color));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on colors's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX objects_idx ON objects (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names indexes per schema, where MySQL names them per table, so the index takes its table's name as a prefix")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM colors WHERE color='green';",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "update or delete on table \"colors\" violates foreign key constraint \"color_fk\" on table \"objects\"", detail: "Key (color)=(green) is still referenced from table \"objects\".", schema: "public", table: "objects", constraint: "color_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM colors;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("red")],
                            &[T("2"), T("green")],
                            &[T("3"), T("blue")],
                            &[T("4"), T("purple")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM colors;",
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
            name: "keyless-foreign-keys: updating to null works as expected in commit",
            intent: "Setting one foreign key column to NULL and another to a valid key commits.",
            set_up_script: &[
                "CREATE TABLE parent (id int, v1 int, v2 int); CREATE UNIQUE INDEX v1 ON parent (v1); CREATE UNIQUE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE unprocessed_t (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE additional_t (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t (id int PRIMARY KEY, unprocessed_id int, FOREIGN KEY (unprocessed_id) REFERENCES unprocessed_t(id) ON DELETE CASCADE ON UPDATE CASCADE, additional_id int, FOREIGN KEY (additional_id) REFERENCES additional_t(id) ON DELETE CASCADE ON UPDATE CASCADE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the index on unprocessed_t's referenced columns is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'schema');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO additional_t VALUES (20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (id, additional_id) VALUES (1,20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'initial');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO unprocessed_t VALUES (20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET additional_id = NULL, unprocessed_id = 20 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM additional_t;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'this should not break');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
    ]);
}
