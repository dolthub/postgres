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

//! Dolt's integration-tests/bats/vector-index.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};

#[test]
fn test_vector_index() {
    run_scripts(&[
        ScriptTest {
            name: "vector-index: rebuild smoke test",
            intent: "A vector index finds the nearest row before and after it is rebuilt.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, col1 vector(1) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, '[0]');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_idx ON t USING hnsw (col1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM t ORDER BY col1 <-> '[0]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[0]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX t_idx;",
                    expected: Expected::Tag("REINDEX"),
                    dolt: Some(Dolt::MySqlOnly("dolt index rebuild is the CLI's, so REINDEX rebuilds the index")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, col1 FROM t ORDER BY col1 <-> '[0]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[0]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: CREATE TABLE VECTOR INDEX",
            intent: "A vector index made with its table is listed with its column.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX v1 ON test USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's index clause of CREATE TABLE is CREATE INDEX, and Dolt's vector index is pgvector's HNSW index here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                            &[T("v1"), T("CREATE INDEX v1 ON public.test USING hnsw (v1 vector_l2_ops)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: CREATE TABLE UNIQUE KEY",
            intent: "A unique vector index is refused.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX v1 ON test USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "access method \"hnsw\" does not support unique indexes", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's index clause of CREATE TABLE is CREATE INDEX, and Dolt's vector index is pgvector's HNSW index here, which can't be unique; Dolt skips this test (\"UNIQUE VECTOR not supported\")")),
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: CREATE TABLE INDEX named with comment",
            intent: "A named vector index keeps its comment.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON test USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
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
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.test USING hnsw (v1 vector_l2_ops)")],
                            &[T("test_pkey"), T("CREATE UNIQUE INDEX test_pkey ON public.test USING btree (pk)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
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
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: CREATE VECTOR INDEX then INSERT",
            intent: "A vector index made before the rows finds the nearest row.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[99,51]")],
                            &[T("2"), T("[11,55]")],
                            &[T("3"), T("[88,52]")],
                            &[T("4"), T("[22,54]")],
                            &[T("5"), T("[77,53]")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[77, 53]' LIMIT 1;",
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
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: INSERT then CREATE VECTOR INDEX",
            intent: "A vector index made after the rows finds the nearest row.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[99,51]")],
                            &[T("2"), T("[11,55]")],
                            &[T("3"), T("[88,52]")],
                            &[T("4"), T("[22,54]")],
                            &[T("5"), T("[77,53]")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[77, 53]' LIMIT 1;",
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
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: INSERT then ALTER TABLE CREATE VECTOR INDEX",
            intent: "A vector index added to a filled table finds the nearest row.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... ADD VECTOR INDEX is CREATE INDEX, and Dolt's vector index is pgvector's HNSW index here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[99,51]")],
                            &[T("2"), T("[11,55]")],
                            &[T("3"), T("[88,52]")],
                            &[T("4"), T("[22,54]")],
                            &[T("5"), T("[77,53]")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[99,51]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: ALTER TABLE CREATE VECTOR INDEX unnamed",
            intent: "An unnamed vector index gets a name and is listed.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... ADD VECTOR INDEX is CREATE INDEX, and Dolt's vector index is pgvector's HNSW index here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                            &[T("onepk_v1_idx"), T("CREATE INDEX onepk_v1_idx ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: INSERT then REPLACE",
            intent: "A vector index follows replaced rows.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[98, -1]'), (2, '[11, 55]'), (3, '[87, 52]'), (4, '[102, 54]'), (6, '[77, 53]') ON CONFLICT (pk1) DO UPDATE SET v1 = EXCLUDED.v1;",
                    expected: Expected::Tag("INSERT 0 5"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's REPLACE INTO is an INSERT ... ON CONFLICT DO UPDATE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[98,-1]")],
                            &[T("2"), T("[11,55]")],
                            &[T("3"), T("[87,52]")],
                            &[T("4"), T("[102,54]")],
                            &[T("5"), T("[77,53]")],
                            &[T("6"), T("[77,53]")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[77,53]' LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("6")],
                            &[T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY v1 <-> '[99,51]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("4"), T("[102,54]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[98,-1]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: INSERT then UPDATE",
            intent: "A vector index follows updated vectors.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE onepk SET v1 = ARRAY[(v1::real[])[2], (v1::real[])[1]]::vector WHERE pk1 >= 3;",
                    expected: Expected::Tag("UPDATE 3"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's JSON_ARRAY of the JSON elements is an array of the vector's elements cast to a vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[99,51]")],
                            &[T("2"), T("[11,55]")],
                            &[T("3"), T("[52,88]")],
                            &[T("4"), T("[54,22]")],
                            &[T("5"), T("[53,77]")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[53,77]' LIMIT 1;",
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
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: INSERT then DELETE some",
            intent: "A vector index drops deleted rows.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk WHERE (v1::real[])[1]::int % 2 = 0;",
                    expected: Expected::Tag("DELETE 2"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's JSON element ->> is an element of the vector cast to an array")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[99,51]")],
                            &[T("2"), T("[11,55]")],
                            &[T("5"), T("[77,53]")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[77,53]' LIMIT 1;",
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
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[22,54]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: INSERT then DELETE all",
            intent: "A vector index drops deleted rows down to none.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk WHERE (v1::real[])[1] = 99;",
                    expected: Expected::Tag("DELETE 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's JSON element ->> is an element of the vector cast to an array")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("2"), T("[11,55]")],
                            &[T("3"), T("[88,52]")],
                            &[T("4"), T("[22,54]")],
                            &[T("5"), T("[77,53]")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[22,54]' LIMIT 1;",
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
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[99,51]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM onepk;",
                    expected: Expected::Tag("DELETE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[99,51]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: CREATE INDEX with same name",
            intent: "A failed index creation under a taken name leaves the existing vector index working.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v2 vector_l2_ops);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v2\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1, v1 FROM onepk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("v1", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[99,51]")],
                            &[T("2"), T("[11,55]")],
                            &[T("3"), T("[88,52]")],
                            &[T("4"), T("[22,54]")],
                            &[T("5"), T("[77,53]")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[88,52]' LIMIT 1;",
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
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: CREATE INDEX with same columns",
            intent: "A second vector index on the same column can be made.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1_dup ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("idx_v1_dup"), T("CREATE INDEX idx_v1_dup ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: Disallow 'dolt_' name prefix",
            intent: "Index names starting with dolt_ are refused.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX dolt_idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "dolt_idx_v1", message_contains: true, ..E }),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX dolt_idx_v1 ON onepk (v1);",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "dolt_idx_v1", message_contains: true, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... ADD INDEX is CREATE INDEX")),
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: DROP INDEX",
            intent: "A dropped vector index is no longer listed.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's DROP INDEX ... ON a table is DROP INDEX of the index")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: ALTER TABLE DROP INDEX",
            intent: "A vector index dropped through its table is no longer listed.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Tag("DROP INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... DROP INDEX is DROP INDEX")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: ALTER TABLE RENAME INDEX",
            intent: "Renaming a vector index to a taken name fails, and to a free name succeeds.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v2 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX idx_v1 RENAME TO idx_v2;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"idx_v2\" already exists", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... RENAME INDEX is ALTER INDEX ... RENAME")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX idx_v1 RENAME TO idx_vfirst;",
                    expected: Expected::Tag("ALTER INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... RENAME INDEX is ALTER INDEX ... RENAME")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'onepk' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME), Column("indexdef", TEXT)],
                        rows: &[
                            &[T("idx_v2"), T("CREATE INDEX idx_v2 ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("idx_vfirst"), T("CREATE INDEX idx_vfirst ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: TRUNCATE TABLE",
            intent: "TRUNCATE keeps the vector index and empties it.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
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
                            &[T("idx_v1"), T("CREATE INDEX idx_v1 ON public.onepk USING hnsw (v1 vector_l2_ops)")],
                            &[T("onepk_pkey"), T("CREATE UNIQUE INDEX onepk_pkey ON public.onepk USING btree (pk1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt index ls, dolt index cat, and dolt schema show are the CLI's, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[99,51]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: can GC",
            intent: "A table with a vector index takes rows after garbage collection.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON onepk USING hnsw (v1 vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (1, '[99, 51]'), (2, '[11, 55]'), (3, '[88, 52]'), (4, '[22, 54]'), (5, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO onepk VALUES (6, '[99, 51]'), (7, '[11, 55]'), (8, '[88, 52]'), (9, '[22, 54]'), (10, '[77, 53]');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk1 FROM onepk ORDER BY v1 <-> '[11, 55]' LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8)],
                        rows: &[
                            &[T("7")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
        ScriptTest {
            name: "vector-index: can GC with out-of-line vectors",
            intent: "A vector index over vectors stored out of line still plans and finds the nearest row after garbage collection.",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE onepk (pk1 bigint PRIMARY KEY, v1 vector(2) NOT NULL);",
                "CREATE TABLE twopk (pk1 bigint, pk2 bigint, v1 vector(2) NOT NULL, PRIMARY KEY(pk1, pk2));",
                "SELECT dolt_add('.');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE big (pk bigint PRIMARY KEY, v vector(600) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's vector index over JSON arrays and VEC_DISTANCE are its own, so the port uses pgvector's vector type, an HNSW index with vector_l2_ops, and its <-> distance")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO big VALUES (1, '[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600]'), (2, '[101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600,601,602,603,604,605,606,607,608,609,610,611,612,613,614,615,616,617,618,619,620,621,622,623,624,625,626,627,628,629,630,631,632,633,634,635,636,637,638,639,640,641,642,643,644,645,646,647,648,649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,697,698,699,700]'), (3, '[201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600,601,602,603,604,605,606,607,608,609,610,611,612,613,614,615,616,617,618,619,620,621,622,623,624,625,626,627,628,629,630,631,632,633,634,635,636,637,638,639,640,641,642,643,644,645,646,647,648,649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,697,698,699,700,701,702,703,704,705,706,707,708,709,710,711,712,713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,777,778,779,780,781,782,783,784,785,786,787,788,789,790,791,792,793,794,795,796,797,798,799,800]');",
                    expected: Expected::Tag("INSERT 0 3"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's STRING_TO_VECTOR is a cast of the text to pgvector's vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v ON big USING hnsw (v vector_l2_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT pk FROM big ORDER BY v <-> '[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Index Scan using idx_v on big")],
                            &[T("        Order By: (v <-> '[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600]'::vector)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::Differs("go-mysql-server's plans name an index read IndexedTableAccess, and Postgres' EXPLAIN shows the Index Scan")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM big ORDER BY v <-> '[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,382,383,384,385,386,387,388,389,390,391,392,393,394,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,410,411,412,413,414,415,416,417,418,419,420,421,422,423,424,425,426,427,428,429,430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,510,511,512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,557,558,559,560,561,562,563,564,565,566,567,568,569,570,571,572,573,574,575,576,577,578,579,580,581,582,583,584,585,586,587,588,589,590,591,592,593,594,595,596,597,598,599,600]' LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("pgvector isn't ported (REBUILD_PLAN, pgvector and PostGIS)"),
            ..S
        },
    ]);
}
