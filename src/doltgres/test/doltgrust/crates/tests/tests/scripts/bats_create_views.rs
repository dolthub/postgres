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

//! Dolt's integration-tests/bats/create-views.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_create_views() {
    run_scripts(&[
        ScriptTest {
            name: "create-views: create a single view",
            intent: "A view can be created and read.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW four AS SELECT 2+2 AS res;",
                    expected: Expected::Tag("CREATE VIEW"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FROM dual is no FROM clause")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM four;",
                    expected: Expected::Rows {
                        columns: &[Column("res", INT4)],
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
            name: "create-views: drop a single view",
            intent: "A view can be created and dropped.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW four AS SELECT 2+2 AS res;",
                    expected: Expected::Tag("CREATE VIEW"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FROM dual is no FROM clause")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW four;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: drop and create same view",
            intent: "Dropping a committed view and creating it again identically leaves no change.",
            set_up_script: &[
                "CREATE VIEW four AS SELECT 2+2 AS res;",
                "CREATE VIEW six AS SELECT 3+3 AS res;",
                "SELECT dolt_commit('-Am', 'new views');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP VIEW four;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW four AS SELECT 2+2 AS res;",
                    expected: Expected::Tag("CREATE VIEW"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FROM dual is no FROM clause")),
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
                    dolt: Some(Dolt::MySqlOnly("dolt diff with no output is a clean dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: join two views",
            intent: "Two views can be joined.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW four AS SELECT 2+2 AS res;",
                    expected: Expected::Tag("CREATE VIEW"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FROM dual is no FROM clause")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW curr AS SELECT now();",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT res FROM four, curr;",
                    expected: Expected::Rows {
                        columns: &[Column("res", INT4)],
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
            name: "create-views: cannot create view referencing non-existent table",
            intent: "A view of a missing table is refused.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW broken AS SELECT id FROM my_users;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"my_users\" does not exist", position: 38, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: can create view with escaped name",
            intent: "Views and tables with quoted names can be created and read.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE \"my-users\" (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"my-users\" (id) VALUES (0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW \"will-work\" AS SELECT id FROM \"my-users\";",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"will-work\";",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
            name: "create-views: can create view referencing table",
            intent: "A view of a table can be created.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE my_users (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW will_work AS SELECT id FROM my_users;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: can drop table with view referencing it",
            intent: "Dropping a table a view depends on behaves as Postgres' dependency rules require.",
            set_up_script: &[
                "CREATE TABLE my_users (id int PRIMARY KEY);",
                "CREATE VIEW will_be_broken AS SELECT id FROM my_users;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE my_users;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table my_users because other objects depend on it", detail: "view will_be_broken depends on table my_users", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses to drop a table a view depends on, so the view never breaks, where Dolt drops the table and leaves the view broken")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: view referencing table selects values present when it was created",
            intent: "A view reads its table's rows, and information_schema.views lists it.",
            set_up_script: &[
                "CREATE TABLE my_users (id int PRIMARY KEY);",
                "INSERT INTO my_users VALUES (1), (2), (3);",
                "CREATE VIEW my_users_view AS SELECT id FROM my_users ORDER BY id ASC;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM my_users_view;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_catalog, table_name, view_definition FROM information_schema.views WHERE table_schema = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("table_catalog", NAME), Column("table_name", NAME), Column("view_definition", VARCHAR)],
                        rows: &[
                            &[T("postgres"), T("my_users_view"), T(" SELECT id\n   FROM my_users\n  ORDER BY id;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres prints a view's definition from its parsed query, where Dolt keeps the statement's text")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: view referencing table selects values inserted after it was created",
            intent: "A view reads rows inserted after it was created.",
            set_up_script: &[
                "CREATE TABLE my_users (id int PRIMARY KEY);",
                "INSERT INTO my_users VALUES (1), (2), (3);",
                "CREATE VIEW my_users_view AS SELECT id FROM my_users ORDER BY id ASC;",
                "INSERT INTO my_users VALUES (4), (5), (6);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM my_users_view;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: select view with alias",
            intent: "A view read through an alias returns its rows.",
            set_up_script: &[
                "CREATE TABLE my_users (id int PRIMARY KEY);",
                "INSERT INTO my_users VALUES (1), (2), (3);",
                "CREATE VIEW my_users_view AS SELECT id FROM my_users ORDER BY id ASC;",
                "INSERT INTO my_users VALUES (4), (5), (6);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT v.* FROM my_users_view AS V;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: selecting from broken view fails",
            intent: "A view's table can't be dropped from under it, so reading the view still works.",
            set_up_script: &[
                "CREATE TABLE my_users (id int PRIMARY KEY);",
                "CREATE VIEW will_be_broken AS SELECT id FROM my_users;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE my_users;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table my_users because other objects depend on it", detail: "view will_be_broken depends on table my_users", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses to drop a table a view depends on, so the view never breaks, where Dolt drops the table and leaves the view broken")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM will_be_broken;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Postgres refuses to drop a table a view depends on, so the view never breaks, where Dolt drops the table and leaves the view broken")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: created view is queryable from next session",
            intent: "A view created in one session is readable in the next.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW testing AS SELECT 2+2;",
                    expected: Expected::Tag("CREATE VIEW"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FROM dual is no FROM clause")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM testing;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "next",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: created view is droppable from next session",
            intent: "A view created in one session is droppable in the next.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW testing AS SELECT 2+2;",
                    expected: Expected::Tag("CREATE VIEW"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FROM dual is no FROM clause")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW testing;",
                    expected: Expected::Tag("DROP VIEW"),
                    client: "next",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM testing;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"testing\" does not exist", position: 15, ..E }),
                    client: "third",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: database with broken view can be used",
            intent: "A view's table can't be dropped from under it, so the database and view stay usable.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE users (id varchar(20) PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW all_users AS SELECT * FROM users;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM all_users;",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE users;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table users because other objects depend on it", detail: "view all_users depends on table users", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses to drop a table a view depends on, so the view never breaks, where Dolt drops the table and leaves the view broken")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 2+2;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's FROM dual is no FROM clause")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM all_users;",
                    expected: Expected::Rows {
                        columns: &[Column("id", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Postgres refuses to drop a table a view depends on, so the view never breaks, where Dolt drops the table and leaves the view broken")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW all_users;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: Use a committed view",
            intent: "A committed view survives a hard reset, which drops an uncommitted one.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW four AS SELECT 2+2 AS res;",
                    expected: Expected::Tag("CREATE VIEW"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FROM dual is no FROM clause")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_commit('-m', 'Checked in a view');",
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
                    query: "SELECT * FROM four;",
                    expected: Expected::Rows {
                        columns: &[Column("res", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW five AS SELECT 2+3 AS res;",
                    expected: Expected::Tag("CREATE VIEW"),
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
                    query: "SELECT * FROM four;",
                    expected: Expected::Rows {
                        columns: &[Column("res", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM five;",
                    expected: Expected::Rows {
                        columns: &[Column("res", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("A view is a schema object of its own (STORAGE_DESIGN IV.3a), so a new uncommitted view is untracked and a hard reset keeps it as it keeps a new table, where Dolt keeps views as rows of its committed dolt_schemas")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW four AS SELECT 2+3 AS res;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM four;",
                    expected: Expected::Rows {
                        columns: &[Column("res", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
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
                    query: "SELECT * FROM four;",
                    expected: Expected::Rows {
                        columns: &[Column("res", INT4)],
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
            name: "create-views: AS OF",
            intent: "A revision database reads a view's definition and data at that revision, and AS OF reads data at the revision through the current definition.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'table with one row');",
                "SELECT dolt_branch('onerow');",
                "INSERT INTO t1 VALUES (2,2);",
                "SELECT dolt_commit('-am', 'table with two rows');",
                "SELECT dolt_branch('tworows');",
                "CREATE VIEW v1 AS SELECT * FROM t1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'view with select *');",
                "SELECT dolt_branch('view');",
                "INSERT INTO t1 VALUES (3,3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'table with three rows');",
                "SELECT dolt_branch('threerows');",
                "DROP VIEW v1;",
                "CREATE VIEW v1 AS SELECT a+10 AS \"a+10\", b+10 AS \"b+10\" FROM t1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/view\".public.v1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 AS OF 'view' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a+10", INT4), Column("b+10", INT4)],
                        rows: &[
                            &[T("11"), T("11")],
                            &[T("12"), T("12")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL names a view's computed columns after their expressions, which the view's aliases do, since Postgres would name both ?column?")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: describe correctly works with complex views",
            intent: "information_schema lists a view's columns and types, for views of one table and of a join.",
            set_up_script: &[
                "CREATE TABLE t(pk int PRIMARY KEY, val int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW view1 AS SELECT * FROM t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'view1' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("integer")],
                            &[T("val"), T("integer")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's; information_schema.columns lists the view's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW view2 AS SELECT pk FROM t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'view2' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2(pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (1,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW view3 AS SELECT t.val AS v1, t2.val AS v2 FROM t INNER JOIN t2 ON t.pk=t2.pk;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'view3' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("v1"), T("integer")],
                            &[T("v2"), T("integer")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: can correctly alter a view",
            intent: "ALTER VIEW ... AS, which Postgres has no form of, is refused.",
            set_up_script: &[
                "CREATE TABLE t(pk int PRIMARY KEY, val int);",
                "CREATE VIEW view1 AS SELECT * FROM t;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER VIEW view1 AS SELECT val FROM t;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"AS\"", position: 18, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER VIEW ... AS has no Postgres form, where CREATE OR REPLACE VIEW changes a view; Dolt skips this test (\"ALTER VIEW is unsupported\")")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create-views: views get properly formatted in the information schema table",
            intent: "information_schema.views prints a view's definition.",
            set_up_script: &[
                "CREATE TABLE t(pk int PRIMARY KEY, val int);",
                "CREATE VIEW view1 AS SELECT pk FROM t;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT view_definition FROM information_schema.views WHERE table_name = 'view1';",
                    expected: Expected::Rows {
                        columns: &[Column("view_definition", VARCHAR)],
                        rows: &[
                            &[T(" SELECT pk\n   FROM t;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres prints a view's definition from its parsed query, where Dolt qualifies its names with the database; Dolt skips this test (\"views are not correctly formatted right now\")")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
