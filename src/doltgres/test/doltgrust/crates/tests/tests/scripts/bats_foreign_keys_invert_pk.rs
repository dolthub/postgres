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

//! Dolt's integration-tests/bats/foreign-keys-invert-pk.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_foreign_keys_invert_pk() {
    run_scripts(&[
        ScriptTest {
            name: r#"foreign-keys-invert-pk: test commit check pass"#,
            intent: r#"A foreign key on the second column of a parent's primary key, which Postgres needs a unique constraint on, passes the commit's checks."#,
            set_up_script: &[
                r#"CREATE TABLE a (x int, y int, PRIMARY KEY (y,x), UNIQUE (y));"#,
                r#"CREATE TABLE b (x int, y int, PRIMARY KEY (y,x), FOREIGN KEY (y) REFERENCES a(y) ON UPDATE CASCADE ON DELETE CASCADE);"#,
                r#"INSERT INTO a VALUES (4,0), (3,1), (2,2);"#,
                r#"INSERT INTO b VALUES (2,1), (4,2), (3,0);"#,
                r#"SELECT dolt_add('.');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'cm');"#,
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"foreign-keys-invert-pk: no secondary indexes made"#,
            intent: r#"A foreign key adds no index of its own to either table."#,
            set_up_script: &[
                r#"CREATE TABLE a (x int, y int, PRIMARY KEY (y,x), UNIQUE (y));"#,
                r#"CREATE TABLE b (x int, y int, PRIMARY KEY (y,x), FOREIGN KEY (y) REFERENCES a(y) ON UPDATE CASCADE ON DELETE CASCADE);"#,
                r#"INSERT INTO a VALUES (4,0), (3,1), (2,2);"#,
                r#"INSERT INTO b VALUES (2,1), (4,2), (3,0);"#,
                r#"SELECT dolt_add('.');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT indexname FROM pg_indexes WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("a_pkey")],
                            &[T("a_y_key")],
                            &[T("b_pkey")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls is a CLI command, so the port lists pg_indexes, where a_y_key is the unique constraint Postgres needs on a foreign key's referenced column, and MySQL references the primary key's leading column instead")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"foreign-keys-invert-pk: check referential integrity on merge"#,
            intent: r#"A merge that brings a child row whose parent is missing records one foreign key violation on the child table."#,
            set_up_script: &[
                r#"CREATE TABLE a (x int, y int, PRIMARY KEY (y,x), UNIQUE (y));"#,
                r#"CREATE TABLE b (x int, y int, PRIMARY KEY (y,x), FOREIGN KEY (y) REFERENCES a(y) ON UPDATE CASCADE ON DELETE CASCADE);"#,
                r#"INSERT INTO a VALUES (4,0), (3,1), (2,2);"#,
                r#"INSERT INTO b VALUES (2,1), (4,2), (3,0);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'main');"#,
                r#"SELECT * FROM dolt_checkout('-b', 'feat');"#,
                r#"SET session_replication_role = replica;"#,
                r#"INSERT INTO b VALUES (1,3);"#,
                r#"RESET session_replication_role;"#,
                r#"SELECT dolt_commit('-am', 'feat');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('feat', '-m', 'merge feat');"#,
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
                    query: r#"SELECT dolt_verify_constraints('--all');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt constraints verify checks the constraints, which dolt_verify_constraints() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.b"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"foreign-keys-invert-pk: test bad insert"#,
            intent: r#"A child row whose parent key is missing is refused."#,
            set_up_script: &[
                r#"CREATE TABLE a (x int, y int, PRIMARY KEY (y,x), UNIQUE (y));"#,
                r#"CREATE TABLE b (x int, y int, PRIMARY KEY (y,x), FOREIGN KEY (y) REFERENCES a(y) ON UPDATE CASCADE ON DELETE CASCADE);"#,
                r#"INSERT INTO a VALUES (4,0), (3,1), (2,2);"#,
                r#"INSERT INTO b VALUES (2,1), (4,2), (3,0);"#,
                r#"SELECT dolt_add('.');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (1,3);"#,
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "b" violates foreign key constraint "b_y_fkey""#, detail: "Key (y)=(3) is not present in table \"a\".", schema: "public", table: "b", constraint: "b_y_fkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"foreign-keys-invert-pk: test update"#,
            intent: r#"An update of a parent's key cascades to its children."#,
            set_up_script: &[
                r#"CREATE TABLE a (x int, y int, PRIMARY KEY (y,x), UNIQUE (y));"#,
                r#"CREATE TABLE b (x int, y int, PRIMARY KEY (y,x), FOREIGN KEY (y) REFERENCES a(y) ON UPDATE CASCADE ON DELETE CASCADE);"#,
                r#"INSERT INTO a VALUES (4,0), (3,1), (2,2);"#,
                r#"INSERT INTO b VALUES (2,1), (4,2), (3,0);"#,
                r#"SELECT dolt_add('.');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"UPDATE a SET y = -1 WHERE y = 0;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM b;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("3"), T("-1")],
                            &[T("2"), T("1")],
                            &[T("4"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"foreign-keys-invert-pk: test delete"#,
            intent: r#"A delete of a parent cascades to its children."#,
            set_up_script: &[
                r#"CREATE TABLE a (x int, y int, PRIMARY KEY (y,x), UNIQUE (y));"#,
                r#"CREATE TABLE b (x int, y int, PRIMARY KEY (y,x), FOREIGN KEY (y) REFERENCES a(y) ON UPDATE CASCADE ON DELETE CASCADE);"#,
                r#"INSERT INTO a VALUES (4,0), (3,1), (2,2);"#,
                r#"INSERT INTO b VALUES (2,1), (4,2), (3,0);"#,
                r#"SELECT dolt_add('.');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DELETE FROM a WHERE y = 0;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM b;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("4"), T("2")],
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
