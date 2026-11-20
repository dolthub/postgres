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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_foreign_keys.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_foreign_key_tests() {
    run_scripts(&[
        ScriptTest {
            name: "UPDATE on table with BINARY primary key, explicit secondary index, and foreign key",
            intent: "Updating a table with a binary primary key, a secondary index, and a foreign key updates every row.",
            set_up_script: &[
                "CREATE TABLE a (a_id int NOT NULL PRIMARY KEY);",
                "CREATE TABLE b (b_id bytea NOT NULL PRIMARY KEY, a_id int NOT NULL, val int NOT NULL, FOREIGN KEY (a_id) REFERENCES a (a_id)); CREATE INDEX a_id ON b (a_id);",
                "INSERT INTO a (a_id) VALUES (1);",
                "INSERT INTO b (b_id, a_id, val) VALUES ('\\x3c'::bytea, 1, 0), ('\\x52'::bytea, 1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE b SET val = 2;",
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CONSTRAINT keyword without a name is left out of the foreign key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM b ORDER BY b_id;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4)],
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
            name: "UPDATE on table with BINARY or VARBINARY primary key and foreign key",
            intent: "Updates of a table with a binary primary key and a foreign key change values and parent keys.",
            set_up_script: &[
                "CREATE TABLE parent (id int NOT NULL PRIMARY KEY);",
                "CREATE TABLE bin_child (id bytea NOT NULL PRIMARY KEY, parent_id int, val int, CONSTRAINT fk_bin FOREIGN KEY (parent_id) REFERENCES parent(id));",
                "CREATE TABLE varbin_child (id bytea NOT NULL PRIMARY KEY, parent_id int, val int, CONSTRAINT fk_varbin FOREIGN KEY (parent_id) REFERENCES parent(id));",
                "INSERT INTO parent VALUES (1), (2);",
                "INSERT INTO bin_child VALUES ('\\x00000000000000000000000000000001'::bytea, 1, 10);",
                "INSERT INTO varbin_child VALUES ('\\x0000000000000001'::bytea, 1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE bin_child SET val = 10 WHERE id = '\\x00000000000000000000000000000001'::bytea;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE bin_child SET val = 20 WHERE id = '\\x00000000000000000000000000000001'::bytea;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM bin_child WHERE id = '\\x00000000000000000000000000000001'::bytea;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE bin_child SET parent_id = 2 WHERE id = '\\x00000000000000000000000000000001'::bytea;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parent_id FROM bin_child WHERE id = '\\x00000000000000000000000000000001'::bytea;",
                    expected: Expected::Rows {
                        columns: &[Column("parent_id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE varbin_child SET val = 20 WHERE id = '\\x0000000000000001'::bytea;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM varbin_child WHERE id = '\\x0000000000000001'::bytea;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE on table with composite BINARY primary key and foreign key",
            intent: "An update of a table with a composite binary primary key and a foreign key changes the row.",
            set_up_script: &[
                "CREATE TABLE parent (id int NOT NULL PRIMARY KEY);",
                "CREATE TABLE child (id1 bytea NOT NULL, id2 bytea NOT NULL, parent_id int, val int, PRIMARY KEY (id1, id2), CONSTRAINT fk_parent FOREIGN KEY (parent_id) REFERENCES parent(id));",
                "INSERT INTO parent VALUES (1);",
                "INSERT INTO child VALUES ('\\x0000000000000001'::bytea, '\\x0000000000000002'::bytea, 1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE child SET val = 20 WHERE id1 = '\\x0000000000000001'::bytea AND id2 = '\\x0000000000000002'::bytea;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM child WHERE id1 = '\\x0000000000000001'::bytea AND id2 = '\\x0000000000000002'::bytea;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE on table where the FK column is BINARY",
            intent: "An update of a table whose foreign key column is binary changes the row.",
            set_up_script: &[
                "CREATE TABLE parent (id bytea NOT NULL PRIMARY KEY);",
                "CREATE TABLE child (id int NOT NULL PRIMARY KEY, parent_id bytea, val int, CONSTRAINT fk_parent FOREIGN KEY (parent_id) REFERENCES parent(id));",
                "INSERT INTO parent VALUES ('\\x00000000000000000000000000000001'::bytea);",
                "INSERT INTO child VALUES (1, '\\x00000000000000000000000000000001'::bytea, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE child SET val = 20 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM child WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "renaming an index keeps foreign keys backed by that index valid",
            intent: "Renaming the indexes a table's foreign keys use keeps the foreign keys enforced.",
            set_up_script: &[
                "CREATE TABLE parent (id int NOT NULL PRIMARY KEY, u int); CREATE UNIQUE INDEX u_idx ON parent (u);",
                "CREATE TABLE child (id int NOT NULL PRIMARY KEY, a int NOT NULL, b int, CONSTRAINT a_fk FOREIGN KEY (a) REFERENCES parent (id), CONSTRAINT b_fk FOREIGN KEY (b) REFERENCES parent (u)); CREATE INDEX a_idx ON child (a); CREATE INDEX b_idx ON child (b);",
                "INSERT INTO parent VALUES (1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER INDEX a_idx RENAME TO a_idx_new;",
                    expected: Expected::Tag("ALTER INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... RENAME INDEX is ALTER INDEX ... RENAME TO, with MySQL's case-insensitive index name folded")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX u_idx RENAME TO u_idx_new;",
                    expected: Expected::Tag("ALTER INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... RENAME INDEX is ALTER INDEX ... RENAME TO, with MySQL's case-insensitive index name folded")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'renamed indexes');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1, 10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (2, 2, 10);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"a_fk\"", detail: "Key (a)=(2) is not present in table \"parent\".", schema: "public", table: "child", constraint: "a_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (3, 1, 20);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"b_fk\"", detail: "Key (b)=(20) is not present in table \"parent\".", schema: "public", table: "child", constraint: "b_fk", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
