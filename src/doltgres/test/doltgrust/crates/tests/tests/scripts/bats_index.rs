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

//! Dolt's integration-tests/bats/index.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_index() {
    run_scripts(&[
        ScriptTest {
            name: "index: rebuild smoke test",
            intent: "Rebuilding a unique index keeps its rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 100);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT col1, pk FROM t ORDER BY col1, pk;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", INT4), Column("pk", INT4)],
                        rows: &[
                            &[T("100"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX t_col1_key;",
                    expected: Expected::Tag("REINDEX"),
                    dolt: Some(Dolt::Differs("dolt index rebuild is REINDEX, and Postgres names a column's UNIQUE index <table>_<column>_key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT col1, pk FROM t ORDER BY col1, pk;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", INT4), Column("pk", INT4)],
                        rows: &[
                            &[T("100"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: Permissive index names",
            intent: "Index names with unusual characters are kept.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX _idx_name_ ON test (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX \"if$\" ON test (v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("_idx_name_"), T("CREATE INDEX _idx_name_ ON public.test USING btree (v1)")],
                            &[T("if$"), T("CREATE INDEX \"if$\" ON public.test USING btree (v2)")],
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: CREATE TABLE INDEX",
            intent: "Indexes declared in CREATE TABLE are made.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON test (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                            &[T("test_v1_idx"), T("CREATE INDEX test_v1_idx ON public.test USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                            &[T("test_v1_idx"), T("CREATE INDEX test_v1_idx ON public.test USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: CREATE TABLE UNIQUE KEY",
            intent: "Unique keys declared in CREATE TABLE are made.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX  ON test (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2(pk bigint PRIMARY KEY, v1 bigint, v2 bigint, UNIQUE (v1));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                            &[T("test_v1_idx"), T("CREATE UNIQUE INDEX test_v1_idx ON public.test USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                            &[T("test_v1_idx"), T("CREATE UNIQUE INDEX test_v1_idx ON public.test USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test2' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test2_pkey"), T("CREATE UNIQUE INDEX test2_pkey ON public.test2 USING btree (pk)")],
                            &[T("test2_v1_key"), T("CREATE UNIQUE INDEX test2_v1_key ON public.test2 USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test2' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test2_pkey"), T("CREATE UNIQUE INDEX test2_pkey ON public.test2 USING btree (pk)")],
                            &[T("test2_v1_key"), T("CREATE UNIQUE INDEX test2_v1_key ON public.test2 USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: CREATE TABLE INDEX named with comment",
            intent: "A commented index declared in CREATE TABLE keeps its comment.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON test (v1, v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON INDEX idx_v1 IS 'hello there';",
                    expected: Expected::Tag("COMMENT"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's index COMMENT clause is COMMENT ON INDEX")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.test USING btree (v1, v2)")],
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.test USING btree (v1, v2)")],
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('idx_v1'::regclass, 'pg_class');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("hello there")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints the index's comment, which obj_description reads")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: CREATE TABLE INDEX multiple",
            intent: "Several indexes declared in CREATE TABLE are made.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON test (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON test (v1, v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                            &[T("test_v1_idx"), T("CREATE INDEX test_v1_idx ON public.test USING btree (v1)")],
                            &[T("test_v1_v2_idx"), T("CREATE INDEX test_v1_v2_idx ON public.test USING btree (v1, v2)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                            &[T("test_v1_idx"), T("CREATE INDEX test_v1_idx ON public.test USING btree (v1)")],
                            &[T("test_v1_v2_idx"), T("CREATE INDEX test_v1_v2_idx ON public.test USING btree (v1, v2)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: CREATE INDEX then INSERT",
            intent: "Rows inserted after an index is made show in it.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("61"), T("52"), T("3"), T("88")],
                            &[T("61"), T("53"), T("5"), T("77")],
                            &[T("63"), T("51"), T("1"), T("99")],
                            &[T("64"), T("55"), T("2"), T("11")],
                            &[T("65"), T("54"), T("4"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 61 AND v1 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("5"), T("77")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: INSERT then CREATE INDEX",
            intent: "An index made after rows are inserted holds them.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("61"), T("52"), T("3"), T("88")],
                            &[T("61"), T("53"), T("5"), T("77")],
                            &[T("63"), T("51"), T("1"), T("99")],
                            &[T("64"), T("55"), T("2"), T("11")],
                            &[T("65"), T("54"), T("4"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 61 AND v1 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("5"), T("77")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: INSERT then ALTER TABLE CREATE INDEX",
            intent: "An index added with ALTER TABLE after rows are inserted holds them.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk (v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("61"), T("52"), T("3"), T("88")],
                            &[T("61"), T("53"), T("5"), T("77")],
                            &[T("63"), T("51"), T("1"), T("99")],
                            &[T("64"), T("55"), T("2"), T("11")],
                            &[T("65"), T("54"), T("4"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 61 AND v1 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("5"), T("77")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE CREATE INDEX unnamed",
            intent: "An index added without a name gets one.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX ON onepk (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                            &[T("onepk_v1_idx"), T("CREATE INDEX onepk_v1_idx ON public.onepk USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                            &[T("onepk_v1_idx"), T("CREATE INDEX onepk_v1_idx ON public.onepk USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: INSERT then REPLACE",
            intent: "Replaced rows update the indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 98, -1), (2, 11, 55), (3, 87, 52), (4, 102, 54), (6, 77, 53) ON CONFLICT (\"pk1\") DO UPDATE SET \"v1\" = EXCLUDED.\"v1\", \"v2\" = EXCLUDED.\"v2\";",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("77"), T("5")],
                            &[T("77"), T("6")],
                            &[T("87"), T("3")],
                            &[T("98"), T("1")],
                            &[T("102"), T("4")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("77"), T("5")],
                            &[T("77"), T("6")],
                            &[T("87"), T("3")],
                            &[T("98"), T("1")],
                            &[T("102"), T("4")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 22;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 102;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, -1, 63), (2, 11, 55, 64), (3, 87, 59, 60), (4, 102, -4, 65), (6, 77, 13, -59) ON CONFLICT (\"pk1\", \"pk2\") DO UPDATE SET \"v1\" = EXCLUDED.\"v1\", \"v2\" = EXCLUDED.\"v2\";",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("-59"), T("13"), T("6"), T("77")],
                            &[T("60"), T("59"), T("3"), T("87")],
                            &[T("61"), T("52"), T("3"), T("88")],
                            &[T("61"), T("53"), T("5"), T("77")],
                            &[T("63"), T("-1"), T("1"), T("99")],
                            &[T("64"), T("55"), T("2"), T("11")],
                            &[T("65"), T("-4"), T("4"), T("102")],
                            &[T("65"), T("54"), T("4"), T("22")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 61 AND v1 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("5"), T("77")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 63 AND v1 = 51;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 63 AND v1 = -1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("1"), T("99")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: INSERT then UPDATE",
            intent: "Updated rows update the indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE onepk SET v1 = v1 - 1 WHERE pk1 >= 3;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("21"), T("4")],
                            &[T("76"), T("5")],
                            &[T("87"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 76;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE twopk SET v1 = v1 + 4, v2 = v2 - v1 WHERE pk1 <= 3;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("9"), T("56"), T("3"), T("88")],
                            &[T("9"), T("59"), T("2"), T("11")],
                            &[T("12"), T("55"), T("1"), T("99")],
                            &[T("61"), T("53"), T("5"), T("77")],
                            &[T("65"), T("54"), T("4"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 5 AND v1 = 56;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
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
            name: "index: INSERT then DELETE some",
            intent: "Deleted rows leave the indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk WHERE v1 % 2 = 0;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("77"), T("5")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 88;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM twopk WHERE v2 - v1 < 10;",
                    expected: Expected::Tag("DELETE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("63"), T("51"), T("1"), T("99")],
                            &[T("65"), T("54"), T("4"), T("22")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 65 AND v1 = 54;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("4"), T("22")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 61 AND v1 = 52;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
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
            name: "index: INSERT then DELETE all",
            intent: "Deleting every row empties the indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk WHERE v1 != -1;",
                    expected: Expected::Tag("DELETE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 88;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk;",
                    expected: Expected::Tag("DELETE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 88;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM twopk WHERE v1 != -1 AND v1 != -1;",
                    expected: Expected::Tag("DELETE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 61 AND v1 = 52;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM twopk;",
                    expected: Expected::Tag("DELETE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, pk2 FROM twopk WHERE v2 = 61 AND v1 = 52;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8)],
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
            name: "index: CREATE INDEX with same name",
            intent: "A second index with a taken name is refused and the first is kept.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v2);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"idx_v1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, pk1 FROM onepk ORDER BY v2, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("51"), T("1")],
                            &[T("52"), T("3")],
                            &[T("53"), T("5")],
                            &[T("54"), T("4")],
                            &[T("55"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v1, v2);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"idx_v\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, v2, pk1, pk2 FROM twopk ORDER BY v1, v2, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("v2", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("51"), T("63"), T("1"), T("99")],
                            &[T("52"), T("61"), T("3"), T("88")],
                            &[T("53"), T("61"), T("5"), T("77")],
                            &[T("54"), T("65"), T("4"), T("22")],
                            &[T("55"), T("64"), T("2"), T("11")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: CREATE INDEX with same columns",
            intent: "Two indexes on the same columns can both exist.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1_dup ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("idx_v1_dup"), T("CREATE INDEX idx_v1_dup ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("idx_v1_dup"), T("CREATE INDEX idx_v1_dup ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2_v1 ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2_v1_dup ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v2_v1"), T("CREATE INDEX idx_v2_v1 ON public.twopk USING btree (v2, v1)")],
                            &[T("idx_v2_v1_dup"), T("CREATE INDEX idx_v2_v1_dup ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v2_v1"), T("CREATE INDEX idx_v2_v1 ON public.twopk USING btree (v2, v1)")],
                            &[T("idx_v2_v1_dup"), T("CREATE INDEX idx_v2_v1_dup ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: Disallow 'dolt_' name prefix",
            intent: "Index names starting with dolt_ are refused.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX dolt_idx_v1 ON onepk(v1);",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "dolt_idx_v1", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX dolt_idx_v1 ON onepk (v1);",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "dolt_idx_v1", message_contains: true, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: DROP INDEX",
            intent: "Dropped indexes leave the table.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1pk1 ON onepk(v1,pk1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2v1 ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1v2 ON twopk(v1, v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("idx_v1pk1"), T("CREATE INDEX idx_v1pk1 ON public.onepk USING btree (v1, pk1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("idx_v1pk1"), T("CREATE INDEX idx_v1pk1 ON public.onepk USING btree (v1, pk1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.twopk USING btree (v1, v2)")],
                            &[T("idx_v2v1"), T("CREATE INDEX idx_v2v1 ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.twopk USING btree (v1, v2)")],
                            &[T("idx_v2v1"), T("CREATE INDEX idx_v2v1 ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v2v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1pk1"), T("CREATE INDEX idx_v1pk1 ON public.onepk USING btree (v1, pk1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1pk1"), T("CREATE INDEX idx_v1pk1 ON public.onepk USING btree (v1, pk1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.twopk USING btree (v1, v2)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.twopk USING btree (v1, v2)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE DROP INDEX",
            intent: "Indexes dropped with ALTER TABLE leave the table.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1pk1 ON onepk(v1,pk1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2v1 ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1v2 ON twopk(v1, v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v2v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1pk1"), T("CREATE INDEX idx_v1pk1 ON public.onepk USING btree (v1, pk1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1pk1"), T("CREATE INDEX idx_v1pk1 ON public.onepk USING btree (v1, pk1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.twopk USING btree (v1, v2)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.twopk USING btree (v1, v2)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE RENAME INDEX",
            intent: "Renaming an index to a taken name fails, and to a free one works.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1pk1 ON onepk(v1,pk1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX idx_v1 RENAME TO idx_v1pk1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"idx_v1pk1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX idx_v1 RENAME TO idx_vfirst;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1pk1"), T("CREATE INDEX idx_v1pk1 ON public.onepk USING btree (v1, pk1)")],
                            &[T("idx_vfirst"), T("CREATE INDEX idx_vfirst ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1pk1"), T("CREATE INDEX idx_v1pk1 ON public.onepk USING btree (v1, pk1)")],
                            &[T("idx_vfirst"), T("CREATE INDEX idx_vfirst ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: RENAME TABLE",
            intent: "A renamed table keeps its indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1v2 ON onepk(v1,v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk RENAME TO newpk;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO newpk VALUES (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'newpk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.newpk USING btree (v1)")],
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.newpk USING btree (v1, v2)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.newpk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'newpk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.newpk USING btree (v1)")],
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.newpk USING btree (v1, v2)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.newpk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM newpk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, v2, pk1 FROM newpk ORDER BY v1, v2, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("v2", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("55"), T("2")],
                            &[T("22"), T("54"), T("4")],
                            &[T("77"), T("53"), T("5")],
                            &[T("88"), T("52"), T("3")],
                            &[T("99"), T("51"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM newpk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM newpk WHERE v1 = 88 AND v2 = 52;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: dolt table mv",
            intent: "A moved table keeps its indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1v2 ON onepk(v1,v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk RENAME TO newpk;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("dolt table mv has no SQL form, so ALTER TABLE ... RENAME TO moves the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO newpk VALUES (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'newpk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.newpk USING btree (v1)")],
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.newpk USING btree (v1, v2)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.newpk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'newpk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.newpk USING btree (v1)")],
                            &[T("idx_v1v2"), T("CREATE INDEX idx_v1v2 ON public.newpk USING btree (v1, v2)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.newpk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM newpk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, v2, pk1 FROM newpk ORDER BY v1, v2, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("v2", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("55"), T("2")],
                            &[T("22"), T("54"), T("4")],
                            &[T("77"), T("53"), T("5")],
                            &[T("88"), T("52"), T("3")],
                            &[T("99"), T("51"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM newpk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM newpk WHERE v1 = 88 AND v2 = 52;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: DROP TABLE",
            intent: "A dropped table takes its indexes with it.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE onepk;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 = 77;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"onepk\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: dolt table rm",
            intent: "A removed table takes its indexes with it.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE onepk;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::Differs("dolt table rm has no SQL form, so DROP TABLE removes the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 = 77;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"onepk\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: dolt table cp",
            intent: "A copied table gets the indexes and rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE onepk_new (LIKE onepk INCLUDING ALL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("dolt table cp has no SQL form, so CREATE TABLE ... LIKE copies the table with its indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk_new SELECT * FROM onepk;",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::Differs("dolt table cp copies the rows too")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk_new' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("onepk_new_pkey"), T("CREATE UNIQUE INDEX onepk_new_pkey ON public.onepk_new USING btree (pk1)")],
                            &[T("onepk_new_v1_idx"), T("CREATE INDEX onepk_new_v1_idx ON public.onepk_new USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk_new ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk_new' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("onepk_new_pkey"), T("CREATE UNIQUE INDEX onepk_new_pkey ON public.onepk_new USING btree (pk1)")],
                            &[T("onepk_new_v1_idx"), T("CREATE INDEX onepk_new_v1_idx ON public.onepk_new USING btree (v1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk_new WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: TRUNCATE TABLE",
            intent: "Truncating a table empties its indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE onepk;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
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
            name: "index: SELECT = Primary Key",
            intent: "An equality lookup on the primary key finds the row.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 = 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 = 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 = 5 AND pk2 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 = 999 AND pk2 = 22;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 = 1 AND pk2 = 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 = 88 AND pk2 = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT = Secondary Index",
            intent: "An equality lookup on a secondary index finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 = 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 = 54;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("4"), T("22"), T("54")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 = 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 = 64;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 = 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 = 61 AND v1 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 = 111 AND v1 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 = 61 AND v1 = 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 = 61 AND v1 = 54;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 = 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT > Primary Key",
            intent: "A greater-than lookup on the primary key finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("3"), T("88"), T("52")],
                            &[T("4"), T("22"), T("54")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 > 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("4"), T("22"), T("54"), T("65")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 > 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 > 4 AND pk2 > 22;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 > 999 AND pk2 > 11;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 > 2 AND pk2 > 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 > 3 AND pk2 > 99;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT > Secondary Index",
            intent: "A greater-than lookup on a secondary index finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 > 70;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("3"), T("88"), T("52")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 > 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 > 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55")],
                            &[T("4"), T("22"), T("54")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 > 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 > 63;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("4"), T("22"), T("54"), T("65")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 > 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 > 61 AND v1 > 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("4"), T("22"), T("54"), T("65")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 > 111 AND v1 > 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 > 61 AND v1 > 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 > 64 AND v1 > 54;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 > 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("4"), T("22"), T("54"), T("65")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 > 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT < Primary Key",
            intent: "A less-than lookup on the primary key finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("2"), T("11"), T("55")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 < 4;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("3"), T("88"), T("52"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 < 3 AND pk2 < 99;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 < 0 AND pk2 < 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 < 3 AND pk2 < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 < 2 AND pk2 < 22;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT < Secondary Index",
            intent: "A less-than lookup on a secondary index finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 < 99;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55")],
                            &[T("3"), T("88"), T("52")],
                            &[T("4"), T("22"), T("54")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 < 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("3"), T("88"), T("52")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 < 64;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 < 64 AND v1 < 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("3"), T("88"), T("52"), T("61")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 < 0 AND v1 < 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 < 61 AND v1 < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 < 62 AND v1 < 52;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 < 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("3"), T("88"), T("52"), T("61")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT >= Primary Key",
            intent: "A greater-or-equal lookup on the primary key finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 >= 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55")],
                            &[T("3"), T("88"), T("52")],
                            &[T("4"), T("22"), T("54")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 >= 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 >= 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("4"), T("22"), T("54"), T("65")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 >= 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 >= 4 AND pk2 >= 22;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("4"), T("22"), T("54"), T("65")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 >= 999 AND pk2 >= 11;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 >= 2 AND pk2 >= 999;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 >= 4 AND pk2 >= 88;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT >= Secondary Index",
            intent: "A greater-or-equal lookup on a secondary index finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 >= 70;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("3"), T("88"), T("52")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 >= 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 >= 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55")],
                            &[T("4"), T("22"), T("54")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 >= 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 >= 63;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("4"), T("22"), T("54"), T("65")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 >= 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 >= 61 AND v1 >= 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("4"), T("22"), T("54"), T("65")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 >= 111 AND v1 >= 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 >= 61 AND v1 >= 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 >= 65 AND v1 >= 55;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 >= 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("4"), T("22"), T("54"), T("65")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 >= 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT <= Primary Key",
            intent: "A less-or-equal lookup on the primary key finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 <= 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("2"), T("11"), T("55")],
                            &[T("3"), T("88"), T("52")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 <= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 <= 4;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("4"), T("22"), T("54"), T("65")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 <= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 <= 3 AND pk2 <= 99;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("3"), T("88"), T("52"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 <= 0 AND pk2 <= 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 <= 3 AND pk2 <= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 <= 1 AND pk2 <= 88;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT <= Secondary Index",
            intent: "A less-or-equal lookup on a secondary index finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 <= 99;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("2"), T("11"), T("55")],
                            &[T("3"), T("88"), T("52")],
                            &[T("4"), T("22"), T("54")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 <= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 <= 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("3"), T("88"), T("52")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 <= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 <= 64;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 <= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 <= 64 AND v1 <= 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 <= 0 AND v1 <= 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 <= 61 AND v1 <= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 <= 61 AND v1 <= 51;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 <= 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 <= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT BETWEEN Primary Key",
            intent: "A BETWEEN lookup on the primary key finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 BETWEEN 2 AND 4;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55")],
                            &[T("3"), T("88"), T("52")],
                            &[T("4"), T("22"), T("54")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE pk1 BETWEEN 6 AND 9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 BETWEEN 1 AND 4;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("4"), T("22"), T("54"), T("65")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 BETWEEN 6 AND 7;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 BETWEEN 1 AND 3 AND pk2 BETWEEN 10 AND 90;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("3"), T("88"), T("52"), T("61")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 BETWEEN 6 AND 8 AND pk2 BETWEEN 20 AND 80;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 BETWEEN 1 AND 3 AND pk2 BETWEEN 100 AND 111;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE pk1 BETWEEN 3 AND 5 AND pk2 BETWEEN 10 AND 20;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: SELECT BETWEEN Secondary Index",
            intent: "A BETWEEN lookup on a secondary index finds the rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 BETWEEN 11 AND 99;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("2"), T("11"), T("55")],
                            &[T("3"), T("88"), T("52")],
                            &[T("4"), T("22"), T("54")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 BETWEEN 30 AND 70;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 BETWEEN 50 AND 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51")],
                            &[T("3"), T("88"), T("52")],
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 BETWEEN 20 AND 50;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 BETWEEN 0 AND 64;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("2"), T("11"), T("55"), T("64")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 BETWEEN 70 AND 90;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 BETWEEN 60 AND 64 AND v1 BETWEEN 50 AND 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("5"), T("77"), T("53"), T("61")],
                            &[T("1"), T("99"), T("51"), T("63")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 BETWEEN 50 AND 53 AND v1 BETWEEN 50 AND 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 BETWEEN 60 AND 64 AND v1 BETWEEN 60 AND 64;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 BETWEEN 60 AND 62 AND v1 BETWEEN 54 AND 55;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 BETWEEN 51 AND 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("99"), T("51"), T("63")],
                            &[T("3"), T("88"), T("52"), T("61")],
                            &[T("5"), T("77"), T("53"), T("61")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v1 BETWEEN 90 AND 100;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8)],
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
            name: "index: ALTER TABLE ADD COLUMN",
            intent: "Adding a column keeps the indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON twopk(v2, v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO twopk VALUES (1, 99, 51, 63), (2, 11, 55, 64), (3, 88, 52, 61), (4, 22, 54, 65), (5, 77, 53, 61);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk ADD COLUMN v3 bigint;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE twopk ADD COLUMN v3 bigint NOT NULL DEFAULT 7;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1, pk2 FROM twopk ORDER BY v2, v1, pk1, pk2;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("61"), T("52"), T("3"), T("88")],
                            &[T("61"), T("53"), T("5"), T("77")],
                            &[T("63"), T("51"), T("1"), T("99")],
                            &[T("64"), T("55"), T("2"), T("11")],
                            &[T("65"), T("54"), T("4"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'twopk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v"), T("CREATE INDEX idx_v ON public.twopk USING btree (v2, v1)")],
                            &[T("twopk_pkey"), T("CREATE UNIQUE INDEX twopk_pkey ON public.twopk USING btree (pk1, pk2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM twopk WHERE v2 = 61 AND v1 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8), Column("v2", INT8), Column("v3", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53"), T("61"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE CHANGE COLUMN NULL",
            intent: "Changing an indexed column's nullability keeps its indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE onepk_new (pk1 bigint PRIMARY KEY, v1 bigint NOT NULL, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk_new(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2v1 ON onepk_new(v2,v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk_new VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk_new RENAME COLUMN v1 TO vnew;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk_new ALTER COLUMN vnew DROP DEFAULT, ALTER COLUMN vnew TYPE bigint USING vnew::bigint, ALTER COLUMN vnew DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk_new ORDER BY v1, pk1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v1\" does not exist", hint: "Perhaps you meant to reference the column \"onepk_new.v2\".", position: 8, ..E }),
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1 FROM onepk_new ORDER BY v2, v1, pk1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v1\" does not exist", hint: "Perhaps you meant to reference the column \"onepk_new.v2\".", position: 12, ..E }),
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk_new' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk_new USING btree (vnew)")],
                            &[T("idx_v2v1"), T("CREATE INDEX idx_v2v1 ON public.onepk_new USING btree (v2, vnew)")],
                            &[T("onepk_new_pkey"), T("CREATE UNIQUE INDEX onepk_new_pkey ON public.onepk_new USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk_new WHERE vnew = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("vnew", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE MODIFY COLUMN NULL",
            intent: "Modifying an indexed column's nullability keeps its indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE onepk_new (pk1 bigint PRIMARY KEY, v1 bigint NOT NULL, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk_new(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2v1 ON onepk_new(v2,v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk_new VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk_new ALTER COLUMN v1 DROP DEFAULT, ALTER COLUMN v1 TYPE bigint USING v1::bigint, ALTER COLUMN v1 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk_new ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1 FROM onepk_new ORDER BY v2, v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("51"), T("99"), T("1")],
                            &[T("52"), T("88"), T("3")],
                            &[T("53"), T("77"), T("5")],
                            &[T("54"), T("22"), T("4")],
                            &[T("55"), T("11"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk_new' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk_new USING btree (v1)")],
                            &[T("idx_v2v1"), T("CREATE INDEX idx_v2v1 ON public.onepk_new USING btree (v2, v1)")],
                            &[T("onepk_new_pkey"), T("CREATE UNIQUE INDEX onepk_new_pkey ON public.onepk_new USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk_new WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE CHANGE COLUMN NOT NULL",
            intent: "Making an indexed column NOT NULL keeps its indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2v1 ON onepk(v2,v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk RENAME COLUMN v1 TO vnew;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk ALTER COLUMN vnew DROP DEFAULT, ALTER COLUMN vnew TYPE bigint USING vnew::bigint, ALTER COLUMN vnew SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v1\" does not exist", hint: "Perhaps you meant to reference the column \"onepk.v2\".", position: 8, ..E }),
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1 FROM onepk ORDER BY v2, v1, pk1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v1\" does not exist", hint: "Perhaps you meant to reference the column \"onepk.v2\".", position: 12, ..E }),
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (vnew)")],
                            &[T("idx_v2v1"), T("CREATE INDEX idx_v2v1 ON public.onepk USING btree (v2, vnew)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE vnew = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("vnew", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE MODIFY COLUMN NOT NULL",
            intent: "Modifying an indexed column to NOT NULL keeps its indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2v1 ON onepk(v2,v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk ALTER COLUMN v1 DROP DEFAULT, ALTER COLUMN v1 TYPE bigint USING v1::bigint, ALTER COLUMN v1 SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1 FROM onepk ORDER BY v2, v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("51"), T("99"), T("1")],
                            &[T("52"), T("88"), T("3")],
                            &[T("53"), T("77"), T("5")],
                            &[T("54"), T("22"), T("4")],
                            &[T("55"), T("11"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("idx_v2v1"), T("CREATE INDEX idx_v2v1 ON public.onepk USING btree (v2, v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE RENAME COLUMN",
            intent: "Renaming an indexed column keeps its indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2v1 ON onepk(v2,v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk RENAME COLUMN v1 TO vnew;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v1\" does not exist", hint: "Perhaps you meant to reference the column \"onepk.v2\".", position: 8, ..E }),
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, v1, pk1 FROM onepk ORDER BY v2, v1, pk1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v1\" does not exist", hint: "Perhaps you meant to reference the column \"onepk.v2\".", position: 12, ..E }),
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (vnew)")],
                            &[T("idx_v2v1"), T("CREATE INDEX idx_v2v1 ON public.onepk USING btree (v2, vnew)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE vnew = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("vnew", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("77"), T("53")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: ALTER TABLE DROP COLUMN",
            intent: "Dropping a column drops the indexes on it and keeps the others.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2 ON onepk(v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1v2 ON onepk(v1,v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2v1 ON onepk(v2,v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE onepk DROP COLUMN v1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v2"), T("CREATE INDEX idx_v2 ON public.onepk USING btree (v2)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v2"), T("CREATE INDEX idx_v2 ON public.onepk USING btree (v2)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2, pk1 FROM onepk ORDER BY v2, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v2", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("51"), T("1")],
                            &[T("52"), T("3")],
                            &[T("53"), T("5")],
                            &[T("54"), T("4")],
                            &[T("55"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v2 = 53;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("53")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: Two commits, then check previous",
            intent: "An index's contents follow the commit checked out.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'test commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE onepk SET v1 = v1 - 1 WHERE pk1 >= 3;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'test commmit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("21"), T("4")],
                            &[T("76"), T("5")],
                            &[T("87"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 76;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'last_commit', 'HEAD~1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 76;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
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
            name: "index: UNIQUE INSERT, UPDATE, REPLACE",
            intent: "A unique index refuses duplicates from inserts, updates, and replaces.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE UNIQUE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE UNIQUE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (6, 77, 56);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"idx_v1\"", detail: "Key (v1)=(77) already exists.", schema: "public", table: "onepk", constraint: "idx_v1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (6, 78, 56);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE onepk SET v1 = 22 WHERE pk1 = 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"idx_v1\"", detail: "Key (v1)=(22) already exists.", schema: "public", table: "onepk", constraint: "idx_v1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE onepk SET v1 = 23 WHERE pk1 = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (2, 88, 55) ON CONFLICT (\"pk1\") DO UPDATE SET \"v1\" = EXCLUDED.\"v1\", \"v2\" = EXCLUDED.\"v2\";",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"idx_v1\"", detail: "Key (v1)=(88) already exists.", schema: "public", table: "onepk", constraint: "idx_v1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (2, 89, 55) ON CONFLICT (\"pk1\") DO UPDATE SET \"v1\" = EXCLUDED.\"v1\", \"v2\" = EXCLUDED.\"v2\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: UNIQUE allows multiple NULL values",
            intent: "A unique index allows several NULLs.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX  ON test (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX  ON test2 (v1, v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, NULL), (1, NULL), (2, NULL);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test2 VALUES (0, NULL, NULL), (1, NULL, NULL), (2, 1, NULL), (3, 1, NULL), (4, NULL, 1), (5, NULL, 1);",
                    expected: Expected::Tag("INSERT 0 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("0"), Null],
                            &[T("1"), Null],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE v1 IS NULL ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("0"), Null],
                            &[T("1"), Null],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("0"), Null, Null],
                            &[T("1"), Null, Null],
                            &[T("2"), T("1"), Null],
                            &[T("3"), T("1"), Null],
                            &[T("4"), Null, T("1")],
                            &[T("5"), Null, T("1")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: dolt table import -u",
            intent: "Rows updated by an import update the indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53) ON CONFLICT (pk1) DO UPDATE SET v1 = EXCLUDED.v1, v2 = EXCLUDED.v2;",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so SQL loads index_onepk.csv's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: dolt table import -r",
            intent: "Rows replaced by an import replace the indexes' contents.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 98, 50), (2, 10, 54), (3, 87, 51), (4, 21, 53), (5, 76, 52);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so SQL loads index_onepk.csv's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so SQL loads index_onepk.csv's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: UNIQUE dolt table import -u",
            intent: "Rows updated by an import keep a unique index correct.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53) ON CONFLICT (pk1) DO UPDATE SET v1 = EXCLUDED.v1, v2 = EXCLUDED.v2;",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so SQL loads index_onepk.csv's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE UNIQUE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE UNIQUE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: UNIQUE dolt table import -r",
            intent: "Rows replaced by an import keep a unique index correct.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 98, 50), (2, 10, 54), (3, 87, 51), (4, 21, 53), (5, 76, 52);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk;",
                    expected: Expected::Tag("DELETE 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so SQL loads index_onepk.csv's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so SQL loads index_onepk.csv's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE UNIQUE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE UNIQUE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk;",
                    expected: Expected::Tag("DELETE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk;",
                    expected: Expected::Tag("DELETE 0"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so SQL loads index_onepk.csv's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::Differs("dolt table import has no SQL form, so SQL loads index_onepk.csv's rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: Merge without conflicts",
            intent: "A merge without conflicts brings the other branch's rows into the indexes.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk(v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'baseline commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, 33, 303), (4, 44, 404);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'main changes');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (5, 55, 505), (6, 66, 606), (7, 77, 707), (8, 88, 808);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'other changes');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("1")],
                            &[T("22"), T("2")],
                            &[T("33"), T("3")],
                            &[T("44"), T("4")],
                            &[T("55"), T("5")],
                            &[T("66"), T("6")],
                            &[T("77"), T("7")],
                            &[T("88"), T("8")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onepk WHERE v1 = 55;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("5"), T("55"), T("505")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: Merge into branch with index from branch without index",
            intent: "A merge into a branch with an index fills the index with the merged rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'baseline');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'baseline');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX abc ON test (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added index');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE v1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1 FROM test ORDER BY v1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: Merge into branch without index from branch with index",
            intent: "A merge bringing a new index fills it with the merged rows.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'baseline');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'baseline');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX abc ON test (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added index');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE v1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1 FROM test ORDER BY v1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: INSERT IGNORE INTO with unique key violations ignores correctly",
            intent: "INSERT IGNORE skips rows a unique index refuses.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable(pk int PRIMARY KEY, col1 int UNIQUE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (2,1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"mytable_col1_key\"", detail: "Key (col1)=(1) already exists.", schema: "public", table: "mytable", constraint: "mytable_col1_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM mytable;",
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
                    query: "SELECT * FROM mytable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (2,1) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's INSERT IGNORE warns 1062 for each row it skips, which Postgres' ON CONFLICT DO NOTHING doesn't")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (2,1) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM mytable;",
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
                    query: "SELECT * FROM mytable;",
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
            name: "index: indexes should not be used when functions modify the column",
            intent: "A filter on a function of an indexed column finds the rows the function matches.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (id int, c1 varchar(255), PRIMARY KEY(id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'Aaaa'), (2, 'aAaa');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Aaaa")],
                            &[T("2"), T("aAaa")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE lower(c1)='aaaa';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Aaaa")],
                            &[T("2"), T("aAaa")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX test_idx ON test (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names relations per schema, so an index can't take its table's name")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE lower(c1)='aaaa';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Aaaa")],
                            &[T("2"), T("aAaa")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (id int, c1 varchar(255), c2 varchar(255), PRIMARY KEY(id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test2 VALUES (1, 'Aaaa', 'Bbbb'), (2, 'aAaa', 'bBbb');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Aaaa"), T("Bbbb")],
                            &[T("2"), T("aAaa"), T("bBbb")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2 WHERE lower(c1)='aaaa' AND lower(c2)='bbbb';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Aaaa"), T("Bbbb")],
                            &[T("2"), T("aAaa"), T("bBbb")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX test2_idx ON test2 (c1,c2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres names relations per schema, so an index can't take its table's name")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2 WHERE lower(c1)='aaaa' AND lower(c2)='bbbb';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Aaaa"), T("Bbbb")],
                            &[T("2"), T("aAaa"), T("bBbb")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: alter table create index for different database",
            intent: "An index can be added to a table named with its database.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE public;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE public.test (pk integer NOT NULL, c1 integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE public.test ADD CONSTRAINT index_test_pkey PRIMARY KEY (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX index_test_c1_idx ON public.test (c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("index_test_c1_idx"), T("CREATE INDEX index_test_c1_idx ON public.test USING btree (c1)")],
                            &[T("index_test_pkey"), T("CREATE UNIQUE INDEX index_test_pkey ON public.test USING btree (pk)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: multiddl alter statement",
            intent: "An ALTER TABLE that drops one index and adds another does both.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, 99, 51), (2, 11, 55), (3, 88, 52), (4, 22, 54), (5, 77, 53);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX myidx ON onepk (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("myidx"), T("CREATE INDEX myidx ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt index ls has no SQL form, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1, pk1 FROM onepk ORDER BY v1, pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T("11"), T("2")],
                            &[T("22"), T("4")],
                            &[T("77"), T("5")],
                            &[T("88"), T("3")],
                            &[T("99"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt index cat has no SQL form, so a query ordered by the index's columns and the primary key reads what the index holds")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("myidx"), T("CREATE INDEX myidx ON public.onepk USING btree (v1)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk WHERE v1 = 77;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index: multiddl drop column add index fails",
            intent: "An ALTER TABLE that drops a column and indexes it fails and changes nothing.",
            set_up_script: &[
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 bigint, v2 bigint, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int PRIMARY KEY, v1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP COLUMN v1, ADD INDEX myidx (v1);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"myidx\" does not exist", position: 41, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ADD INDEX clause in a multi-clause ALTER TABLE has no Postgres form, so the statement fails as Dolt's does and v1 stays")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("t_pkey"), T("CREATE UNIQUE INDEX t_pkey ON public.t USING btree (pk)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"index: Merge abort"#,
            intent: r#"An index holds the merge's rows while conflicts stand, and main's rows again after dolt_merge --abort."#,
            set_up_script: &[
                r#"CREATE TABLE onepk (pk1 BIGINT PRIMARY KEY, v1 BIGINT, v2 BIGINT);"#,
                r#"CREATE TABLE twopk (pk1 BIGINT, pk2 BIGINT, v1 BIGINT, v2 BIGINT, PRIMARY KEY (pk1, pk2));"#,
                r#"SELECT dolt_add('.');"#,
                r#"CREATE INDEX idx_v1 ON onepk (v1);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'baseline commit');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, -33, 33), (4, 44, 404);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'main changes');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO onepk VALUES (1, -11, 11), (2, -22, 22), (3, -33, 33), (4, -44, 44), (5, -55, 55);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'other changes');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"dolt index cat lists an index's entries, so the port reads idx_v1 through an index scan ordered by v1"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1, pk1 FROM onepk WHERE v1 IS NOT NULL ORDER BY v1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T(r#"-55"#), T(r#"5"#)],
                            &[T(r#"-33"#), T(r#"3"#)],
                            &[T(r#"11"#), T(r#"1"#)],
                            &[T(r#"22"#), T(r#"2"#)],
                            &[T(r#"44"#), T(r#"4"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"0"#), T(r#"merge aborted"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1, pk1 FROM onepk WHERE v1 IS NOT NULL ORDER BY v1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T(r#"-33"#), T(r#"3"#)],
                            &[T(r#"11"#), T(r#"1"#)],
                            &[T(r#"22"#), T(r#"2"#)],
                            &[T(r#"44"#), T(r#"4"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"index: Merge resolving all OURS"#,
            intent: r#"Resolving every conflict with --ours leaves the index with our rows and the merge's new row."#,
            set_up_script: &[
                r#"CREATE TABLE onepk (pk1 BIGINT PRIMARY KEY, v1 BIGINT, v2 BIGINT);"#,
                r#"CREATE TABLE twopk (pk1 BIGINT, pk2 BIGINT, v1 BIGINT, v2 BIGINT, PRIMARY KEY (pk1, pk2));"#,
                r#"SELECT dolt_add('.');"#,
                r#"CREATE INDEX idx_v1 ON onepk (v1);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'baseline commit');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, -33, 33), (4, 44, 404);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'main changes');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO onepk VALUES (1, -11, 11), (2, -22, 22), (3, -33, 33), (4, -44, 44), (5, -55, 55);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'other changes');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'onepk');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"dolt index cat lists an index's entries, so the port reads idx_v1 through an index scan ordered by v1"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1, pk1 FROM onepk WHERE v1 IS NOT NULL ORDER BY v1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T(r#"-55"#), T(r#"5"#)],
                            &[T(r#"-33"#), T(r#"3"#)],
                            &[T(r#"11"#), T(r#"1"#)],
                            &[T(r#"22"#), T(r#"2"#)],
                            &[T(r#"44"#), T(r#"4"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"index: Merge resolving all OURS with stored procedure"#,
            intent: r#"Resolving every conflict with dolt_conflicts_resolve --ours leaves the index with our rows and the merge's new row."#,
            set_up_script: &[
                r#"CREATE TABLE onepk (pk1 BIGINT PRIMARY KEY, v1 BIGINT, v2 BIGINT);"#,
                r#"CREATE TABLE twopk (pk1 BIGINT, pk2 BIGINT, v1 BIGINT, v2 BIGINT, PRIMARY KEY (pk1, pk2));"#,
                r#"SELECT dolt_add('.');"#,
                r#"CREATE INDEX idx_v1 ON onepk (v1);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'baseline commit');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, -33, 33), (4, 44, 404);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'main changes');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO onepk VALUES (1, -11, 11), (2, -22, 22), (3, -33, 33), (4, -44, 44), (5, -55, 55);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'other changes');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'onepk');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"dolt index cat lists an index's entries, so the port reads idx_v1 through an index scan ordered by v1"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1, pk1 FROM onepk WHERE v1 IS NOT NULL ORDER BY v1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T(r#"-55"#), T(r#"5"#)],
                            &[T(r#"-33"#), T(r#"3"#)],
                            &[T(r#"11"#), T(r#"1"#)],
                            &[T(r#"22"#), T(r#"2"#)],
                            &[T(r#"44"#), T(r#"4"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"index: Merge resolving all THEIRS"#,
            intent: r#"Resolving every conflict with --theirs leaves the index with their rows."#,
            set_up_script: &[
                r#"CREATE TABLE onepk (pk1 BIGINT PRIMARY KEY, v1 BIGINT, v2 BIGINT);"#,
                r#"CREATE TABLE twopk (pk1 BIGINT, pk2 BIGINT, v1 BIGINT, v2 BIGINT, PRIMARY KEY (pk1, pk2));"#,
                r#"SELECT dolt_add('.');"#,
                r#"CREATE INDEX idx_v1 ON onepk (v1);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'baseline commit');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, -33, 33), (4, 44, 404);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'main changes');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO onepk VALUES (1, -11, 11), (2, -22, 22), (3, -33, 33), (4, -44, 44), (5, -55, 55);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'other changes');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 'onepk');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"dolt index cat lists an index's entries, so the port reads idx_v1 through an index scan ordered by v1"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1, pk1 FROM onepk WHERE v1 IS NOT NULL ORDER BY v1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T(r#"-55"#), T(r#"5"#)],
                            &[T(r#"-44"#), T(r#"4"#)],
                            &[T(r#"-33"#), T(r#"3"#)],
                            &[T(r#"-22"#), T(r#"2"#)],
                            &[T(r#"-11"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"index: Merge resolving all THEIRS with stored procedure"#,
            intent: r#"Resolving every conflict with dolt_conflicts_resolve --theirs leaves the index with their rows."#,
            set_up_script: &[
                r#"CREATE TABLE onepk (pk1 BIGINT PRIMARY KEY, v1 BIGINT, v2 BIGINT);"#,
                r#"CREATE TABLE twopk (pk1 BIGINT, pk2 BIGINT, v1 BIGINT, v2 BIGINT, PRIMARY KEY (pk1, pk2));"#,
                r#"SELECT dolt_add('.');"#,
                r#"CREATE INDEX idx_v1 ON onepk (v1);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'baseline commit');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, -33, 33), (4, 44, 404);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'main changes');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO onepk VALUES (1, -11, 11), (2, -22, 22), (3, -33, 33), (4, -44, 44), (5, -55, 55);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'other changes');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 'onepk');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"dolt index cat lists an index's entries, so the port reads idx_v1 through an index scan ordered by v1"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1, pk1 FROM onepk WHERE v1 IS NOT NULL ORDER BY v1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T(r#"-55"#), T(r#"5"#)],
                            &[T(r#"-44"#), T(r#"4"#)],
                            &[T(r#"-33"#), T(r#"3"#)],
                            &[T(r#"-22"#), T(r#"2"#)],
                            &[T(r#"-11"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"index: Merge individually resolving OURS/THEIRS"#,
            intent: r#"Resolving conflicts one by one, taking their values for some rows, keeps the index in step with the table."#,
            set_up_script: &[
                r#"CREATE TABLE onepk (pk1 BIGINT PRIMARY KEY, v1 BIGINT, v2 BIGINT);"#,
                r#"CREATE TABLE twopk (pk1 BIGINT, pk2 BIGINT, v1 BIGINT, v2 BIGINT, PRIMARY KEY (pk1, pk2));"#,
                r#"SELECT dolt_add('.');"#,
                r#"CREATE INDEX idx_v1 ON onepk (v1);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'baseline commit');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, -33, 33), (4, 44, 404);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'main changes');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO onepk VALUES (1, -11, 11), (2, -22, 22), (3, -33, 33), (4, -44, 44), (5, -55, 55);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'other changes');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_conflicts_onepk WHERE our_pk1 = 4;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE onepk SET v1 = -11, v2 = 11 WHERE pk1 = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE onepk SET v1 = -22, v2 = 22 WHERE pk1 = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_conflicts_onepk WHERE our_pk1 = 1 OR our_pk1 = 2;"#,
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMIT;"#,
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"dolt index cat lists an index's entries, so the port reads idx_v1 through an index scan ordered by v1"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v1, pk1 FROM onepk WHERE v1 IS NOT NULL ORDER BY v1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8), Column("pk1", INT8)],
                        rows: &[
                            &[T(r#"-55"#), T(r#"5"#)],
                            &[T(r#"-33"#), T(r#"3"#)],
                            &[T(r#"-22"#), T(r#"2"#)],
                            &[T(r#"-11"#), T(r#"1"#)],
                            &[T(r#"44"#), T(r#"4"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"index: Merge violates UNIQUE"#,
            intent: r#"A merge whose rows break a unique index records a constraint violation."#,
            set_up_script: &[
                r#"CREATE TABLE onepk (pk1 BIGINT PRIMARY KEY, v1 BIGINT, v2 BIGINT);"#,
                r#"CREATE TABLE twopk (pk1 BIGINT, pk2 BIGINT, v1 BIGINT, v2 BIGINT, PRIMARY KEY (pk1, pk2));"#,
                r#"SELECT dolt_add('.');"#,
                r#"CREATE UNIQUE INDEX idx_v1 ON onepk (v1);"#,
                r#"SELECT dolt_commit('-am', 'baseline commit');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, 33, 303), (4, 44, 404);"#,
                r#"SELECT dolt_commit('-am', 'main changes');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO onepk VALUES (1, 11, 101), (2, 22, 202), (3, 33, 303), (5, 44, 505);"#,
                r#"SELECT dolt_commit('-am', 'other changes');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_force_transaction_commit = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's constraint violations as dolt_force_transaction_commit does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT "table", num_violations FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T(r#"public.onepk"#), T(r#"2"#)],
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
