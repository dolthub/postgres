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

//! Dolt's integration-tests/bats/index-on-writes.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_index_on_writes() {
    run_scripts(&[
        ScriptTest {
            name: "index-on-writes: delete all two_pk",
            intent: "A DELETE that deletes every row of two_pk gives the same rows run once or twice.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk",
            intent: "A DELETE that deletes every row of one_pk gives the same rows run once or twice.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <>, pk",
            intent: "A DELETE that deletes every row of two_pk matched by <> on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <> 1024 AND pk2 <> 1024;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <> 1024 AND pk2 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <> 1024 AND pk2 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 <> 1024 AND pk2 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: ((pk1 <> 1024) AND (pk2 <> 1024))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <>, pk",
            intent: "A DELETE that deletes every row of one_pk matched by <> on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <> 1024;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (pk1 <> 1024)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, >, pk",
            intent: "A DELETE that deletes every row of two_pk matched by > on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 > 0 AND pk2 > 4;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 > 0 AND pk2 > 4;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 > 0 AND pk2 > 4;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 > 0 AND pk2 > 4;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 > 0) AND (pk2 > 4))")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 > 0) AND (pk2 > 4))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, >, pk",
            intent: "A DELETE that deletes every row of one_pk matched by > on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 > 0;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 > 0;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 > 0;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 > 0)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 > 0)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, >=, pk",
            intent: "A DELETE that deletes every row of two_pk matched by >= on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 >= 1 AND pk2 >= 5;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 >= 1 AND pk2 >= 5;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 >= 1 AND pk2 >= 5;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 >= 1 AND pk2 >= 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 >= 1) AND (pk2 >= 5))")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 >= 1) AND (pk2 >= 5))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, >=, pk",
            intent: "A DELETE that deletes every row of one_pk matched by >= on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 >= 1;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 >= 1;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 >= 1;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 >= 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 >= 1)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 >= 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <, pk",
            intent: "A DELETE that deletes every row of two_pk matched by < on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 < 6 AND pk2 < 10;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 < 6 AND pk2 < 10;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 < 6 AND pk2 < 10;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 < 6 AND pk2 < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 < 6) AND (pk2 < 10))")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 < 6) AND (pk2 < 10))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <, pk",
            intent: "A DELETE that deletes every row of one_pk matched by < on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 < 6;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 < 6;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 < 6;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 < 6;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 < 6)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 < 6)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <=, pk",
            intent: "A DELETE that deletes every row of two_pk matched by <= on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <= 5 AND pk2 <= 9;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <= 5 AND pk2 <= 9;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <= 5 AND pk2 <= 9;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 <= 5 AND pk2 <= 9;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 <= 5) AND (pk2 <= 9))")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 <= 5) AND (pk2 <= 9))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <=, pk",
            intent: "A DELETE that deletes every row of one_pk matched by <= on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <= 5;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <= 5;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <= 5;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 <= 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 <= 5)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 <= 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <>, non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by <> on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 <> 1024;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE c1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c1 <> 1024)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <>, non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by <> on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 <> 1024;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE c1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c1 <> 1024)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, >, non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by > on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 > 127;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 > 127;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 > 127;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE c1 > 127;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c1 > 127)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, >, non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by > on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 > 127;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 > 127;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 > 127;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE c1 > 127;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c1 > 127)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, >=, non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by >= on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 >= 128;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 >= 128;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 >= 128;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE c1 >= 128;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c1 >= 128)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, >=, non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by >= on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 >= 128;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 >= 128;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 >= 128;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE c1 >= 128;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c1 >= 128)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <, non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by < on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 < 133;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 < 133;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c1 < 133;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE c1 < 133;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c1 < 133)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <, non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by < on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 < 133;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 < 133;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c1 < 133;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE c1 < 133;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c1 < 133)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <=, non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by <= on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c2 <= 32;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c2 <= 32;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE c2 <= 32;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE c2 <= 32;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c2 <= 32)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <=, non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by <= on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c2 <= 32;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c2 <= 32;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE c2 <= 32;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE c2 <= 32;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c2 <= 32)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <>, pk + non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by <> on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <> 1024 AND pk2 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <> 1024 AND pk2 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <> 1024 AND pk2 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 <> 1024 AND pk2 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: ((pk1 <> 1024) AND (pk2 <> 1024) AND (c1 <> 1024))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <>, pk + non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by <> on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: ((pk1 <> 1024) AND (c1 <> 1024))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, >, pk + non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by > on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 > 0 AND pk2 > 4 AND c1 > 127;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 > 0 AND pk2 > 4 AND c1 > 127;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 > 0 AND pk2 > 4 AND c1 > 127;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 > 0 AND pk2 > 4 AND c1 > 127;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 > 0) AND (pk2 > 4))")],
                            &[T("        Filter: (c1 > 127)")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 > 0) AND (pk2 > 4))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, >, pk + non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by > on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 > 0 AND c1 > 127;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 > 0 AND c1 > 127;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 > 0 AND c1 > 127;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 > 0 AND c1 > 127;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 > 0)")],
                            &[T("        Filter: (c1 > 127)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 > 0)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, >=, pk + non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by >= on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 >= 1 AND pk2 >= 5 AND c1 >= 128;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 >= 1 AND pk2 >= 5 AND c1 >= 128;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 >= 1 AND pk2 >= 5 AND c1 >= 128;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 >= 1 AND pk2 >= 5 AND c1 >= 128;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 >= 1) AND (pk2 >= 5))")],
                            &[T("        Filter: (c1 >= 128)")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 >= 1) AND (pk2 >= 5))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, >=, pk + non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by >= on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 >= 1 AND c1 >= 128;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 >= 1 AND c1 >= 128;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 >= 1 AND c1 >= 128;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 >= 1 AND c1 >= 128;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 >= 1)")],
                            &[T("        Filter: (c1 >= 128)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 >= 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <, pk + non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by < on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 < 6 AND pk2 < 10 AND c1 < 133;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 < 6 AND pk2 < 10 AND c1 < 133;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 < 6 AND pk2 < 10 AND c1 < 133;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 < 6 AND pk2 < 10 AND c1 < 133;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 < 6) AND (pk2 < 10))")],
                            &[T("        Filter: (c1 < 133)")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 < 6) AND (pk2 < 10))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <, pk + non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by < on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 < 6 AND c1 < 133;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 < 6 AND c1 < 133;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 < 6 AND c1 < 133;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 < 6 AND c1 < 133;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 < 6)")],
                            &[T("        Filter: (c1 < 133)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 < 6)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all two_pk, <=, pk + non-pk",
            intent: "A DELETE that deletes every row of two_pk matched by <= on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <= 5 AND pk2 <= 9 AND c2 <= 32;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <= 5 AND pk2 <= 9 AND c2 <= 32;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM two_pk WHERE pk1 <= 5 AND pk2 <= 9 AND c2 <= 32;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM two_pk WHERE pk1 <= 5 AND pk2 <= 9 AND c2 <= 32;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 <= 5) AND (pk2 <= 9))")],
                            &[T("        Filter: (c2 <= 32)")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 <= 5) AND (pk2 <= 9))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: delete all one_pk, <=, pk + non-pk",
            intent: "A DELETE that deletes every row of one_pk matched by <= on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <= 5 AND c2 <= 32;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <= 5 AND c2 <= 32;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 <= 5 AND c2 <= 32;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM one_pk WHERE pk1 <= 5 AND c2 <= 32;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Delete on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 <= 5)")],
                            &[T("        Filter: (c2 <= 32)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 <= 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk",
            intent: "A UPDATE that updates every row of two_pk gives the same rows run once or twice.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk",
            intent: "A UPDATE that updates every row of one_pk gives the same rows run once or twice.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <>, pk",
            intent: "A UPDATE that updates every row of two_pk matched by <> on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <> 1024 AND pk2 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <> 1024 AND pk2 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <> 1024 AND pk2 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 <> 1024 AND pk2 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: ((pk1 <> 1024) AND (pk2 <> 1024))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <>, pk",
            intent: "A UPDATE that updates every row of one_pk matched by <> on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (pk1 <> 1024)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, >, pk",
            intent: "A UPDATE that updates every row of two_pk matched by > on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 > 0 AND pk2 > 4;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 > 0 AND pk2 > 4;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 > 0 AND pk2 > 4;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 > 0 AND pk2 > 4;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 > 0) AND (pk2 > 4))")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 > 0) AND (pk2 > 4))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, >, pk",
            intent: "A UPDATE that updates every row of one_pk matched by > on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 > 0;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 > 0;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 > 0;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 > 0)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 > 0)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, >=, pk",
            intent: "A UPDATE that updates every row of two_pk matched by >= on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 >= 1 AND pk2 >= 5;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 >= 1 AND pk2 >= 5;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 >= 1 AND pk2 >= 5;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 >= 1 AND pk2 >= 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 >= 1) AND (pk2 >= 5))")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 >= 1) AND (pk2 >= 5))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, >=, pk",
            intent: "A UPDATE that updates every row of one_pk matched by >= on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 >= 1;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 >= 1;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 >= 1;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 >= 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 >= 1)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 >= 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <, pk",
            intent: "A UPDATE that updates every row of two_pk matched by < on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 < 6 AND pk2 < 10;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 < 6 AND pk2 < 10;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 < 6 AND pk2 < 10;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 < 6 AND pk2 < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 < 6) AND (pk2 < 10))")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 < 6) AND (pk2 < 10))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <, pk",
            intent: "A UPDATE that updates every row of one_pk matched by < on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 < 6;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 < 6;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 < 6;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 < 6;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 < 6)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 < 6)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <=, pk",
            intent: "A UPDATE that updates every row of two_pk matched by <= on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <= 5 AND pk2 <= 9;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <= 5 AND pk2 <= 9;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <= 5 AND pk2 <= 9;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 <= 5 AND pk2 <= 9;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 <= 5) AND (pk2 <= 9))")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 <= 5) AND (pk2 <= 9))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <=, pk",
            intent: "A UPDATE that updates every row of one_pk matched by <= on the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <= 5;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <= 5;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <= 5;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 <= 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 <= 5)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 <= 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <>, non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by <> on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE c1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c1 <> 1024)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <>, non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by <> on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE c1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c1 <> 1024)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, >, non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by > on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE c1 > 127;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c1 > 127)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, >, non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by > on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE c1 > 127;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c1 > 127)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, >=, non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by >= on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE c1 >= 128;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c1 >= 128)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, >=, non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by >= on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE c1 >= 128;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c1 >= 128)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <, non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by < on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE c1 < 133;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c1 < 133)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <, non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by < on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE c1 < 133;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c1 < 133)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <=, non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by <= on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c2 <= 32;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c2 <= 32;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE c2 <= 32;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE c2 <= 32;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: (c2 <= 32)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <=, non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by <= on columns outside the primary key gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c2 <= 32;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c2 <= 32;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE c2 <= 32;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE c2 <= 32;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: (c2 <= 32)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <>, pk + non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by <> on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <> 1024 AND pk2 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <> 1024 AND pk2 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <> 1024 AND pk2 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 <> 1024 AND pk2 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Seq Scan on two_pk")],
                            &[T("        Filter: ((pk1 <> 1024) AND (pk2 <> 1024) AND (c1 <> 1024))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <>, pk + non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by <> on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 <> 1024 AND c1 <> 1024;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Seq Scan on one_pk")],
                            &[T("        Filter: ((pk1 <> 1024) AND (c1 <> 1024))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, >, pk + non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by > on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 > 0 AND pk2 > 4 AND c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 > 0 AND pk2 > 4 AND c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 > 0 AND pk2 > 4 AND c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 > 0 AND pk2 > 4 AND c1 > 127;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 > 0) AND (pk2 > 4))")],
                            &[T("        Filter: (c1 > 127)")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 > 0) AND (pk2 > 4))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, >, pk + non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by > on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 > 0 AND c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 > 0 AND c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 > 0 AND c1 > 127;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 > 0 AND c1 > 127;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 > 0)")],
                            &[T("        Filter: (c1 > 127)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 > 0)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, >=, pk + non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by >= on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 >= 1 AND pk2 >= 5 AND c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 >= 1 AND pk2 >= 5 AND c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 >= 1 AND pk2 >= 5 AND c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 >= 1 AND pk2 >= 5 AND c1 >= 128;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 >= 1) AND (pk2 >= 5))")],
                            &[T("        Filter: (c1 >= 128)")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 >= 1) AND (pk2 >= 5))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, >=, pk + non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by >= on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 >= 1 AND c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 >= 1 AND c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 >= 1 AND c1 >= 128;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 >= 1 AND c1 >= 128;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 >= 1)")],
                            &[T("        Filter: (c1 >= 128)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 >= 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <, pk + non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by < on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 < 6 AND pk2 < 10 AND c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 < 6 AND pk2 < 10 AND c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 < 6 AND pk2 < 10 AND c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 < 6 AND pk2 < 10 AND c1 < 133;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 < 6) AND (pk2 < 10))")],
                            &[T("        Filter: (c1 < 133)")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 < 6) AND (pk2 < 10))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <, pk + non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by < on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 < 6 AND c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 < 6 AND c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 < 6 AND c1 < 133;",
                    expected: Expected::Tag("UPDATE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 < 6 AND c1 < 133;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 < 6)")],
                            &[T("        Filter: (c1 < 133)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 < 6)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all two_pk, <=, pk + non-pk",
            intent: "A UPDATE that updates every row of two_pk matched by <= on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <= 5 AND pk2 <= 9 AND c2 <= 32;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <= 5 AND pk2 <= 9 AND c2 <= 32;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE two_pk SET c2 = 256 WHERE pk1 <= 5 AND pk2 <= 9 AND c2 <= 32;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("pk2", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("9"), T("128"), T("256")],
                            &[T("2"), T("8"), T("129"), T("256")],
                            &[T("3"), T("7"), T("130"), T("256")],
                            &[T("4"), T("6"), T("131"), T("256")],
                            &[T("5"), T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE two_pk SET c2 = 256 WHERE pk1 <= 5 AND pk2 <= 9 AND c2 <= 32;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on two_pk")],
                            &[T("  ->  Bitmap Heap Scan on two_pk")],
                            &[T("        Recheck Cond: ((pk1 <= 5) AND (pk2 <= 9))")],
                            &[T("        Filter: (c2 <= 32)")],
                            &[T("        ->  Bitmap Index Scan on two_pk_pkey")],
                            &[T("              Index Cond: ((pk1 <= 5) AND (pk2 <= 9))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },

        ScriptTest {
            name: "index-on-writes: update all one_pk, <=, pk + non-pk",
            intent: "A UPDATE that updates every row of one_pk matched by <= on the primary key and another column gives the same rows run once or twice and plans a primary key read only when the condition is on the key.",
            set_up_script: &[
                "CREATE TABLE two_pk (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO two_pk VALUES (1,9,128,32),(2,8,129,31),(3,7,130,30),(4,6,131,29),(5,5,132,28);",
                "CREATE TABLE one_pk (pk1 int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk1));",
                "INSERT INTO one_pk VALUES (1,128,32),(2,129,31),(3,130,30),(4,131,29),(5,132,28);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <= 5 AND c2 <= 32;",
                    expected: Expected::Tag("UPDATE 5"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE and INSERT make the tables with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <= 5 AND c2 <= 32;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c2 = 256 WHERE pk1 <= 5 AND c2 <= 32;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("128"), T("256")],
                            &[T("2"), T("129"), T("256")],
                            &[T("3"), T("130"), T("256")],
                            &[T("4"), T("131"), T("256")],
                            &[T("5"), T("132"), T("256")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE one_pk SET c2 = 256 WHERE pk1 <= 5 AND c2 <= 32;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on one_pk")],
                            &[T("  ->  Bitmap Heap Scan on one_pk")],
                            &[T("        Recheck Cond: (pk1 <= 5)")],
                            &[T("        Filter: (c2 <= 32)")],
                            &[T("        ->  Bitmap Index Scan on one_pk_pkey")],
                            &[T("              Index Cond: (pk1 <= 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan or Bitmap Index Scan, or the Seq Scan of a filter on columns without an index")),
                    skip: Some("Doltgrust reads a primary key that holds the table's rows in key order at correlation 1 (STORAGE_DESIGN Appendix C), so a key range on a table without statistics plans an Index Scan where PG18 plans a Bitmap Heap Scan (8255)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
