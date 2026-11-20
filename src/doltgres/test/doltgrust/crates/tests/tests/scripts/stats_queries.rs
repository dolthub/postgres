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


//! Dolt's go/libraries/doltcore/sqle/enginetest/stats_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Dolt, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_stat_branch_tests() {
    run_scripts(&[
        ScriptTest {
            name: "multi branch stats",
            intent: "Statistics belong to the branch they were gathered on, so each branch keeps its own row counts.",
            set_up_script: &[
                "CREATE TABLE xy (x bigint PRIMARY KEY, y int, z varchar(500));",
                "CREATE INDEX ON xy (y, z);",
                "INSERT INTO xy VALUES (0,0,'a'), (1,0,'a'), (2,0,'a'), (3,0,'a'), (4,1,'a'), (5,2,'a');",
                "SELECT dolt_commit('-Am', 'xy');",
                "SELECT * FROM dolt_checkout('-b','feat');",
                "CREATE TABLE ab (a bigint PRIMARY KEY, b int, c int);",
                "CREATE INDEX ON ab (b, c);",
                "INSERT INTO ab VALUES (0,0,1), (1,0,1), (2,0,1), (3,0,1), (4,1,1), (5,2,1);",
                "SELECT dolt_commit('-Am', 'ab');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE;",
                    expected: Expected::Tag("ANALYZE"),
                    dolt: Some(Dolt::Differs("Dolt gathers statistics in the background into its own store, read from dolt_statistics; Doltgrust keeps Postgres' statistics, gathered by ANALYZE into pg_class and pg_statistic")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltuples FROM pg_class WHERE relname IN ('ab', 'ab_b_c_idx', 'ab_pkey', 'xy', 'xy_pkey', 'xy_y_z_idx') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("xy"), T("6")],
                            &[T("xy_pkey"), T("6")],
                            &[T("xy_y_z_idx"), T("6")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt gathers statistics in the background into its own store, read from dolt_statistics; Doltgrust keeps Postgres' statistics, gathered by ANALYZE into pg_class and pg_statistic")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feat');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feat'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltuples FROM pg_class WHERE relname IN ('ab', 'ab_b_c_idx', 'ab_pkey', 'xy', 'xy_pkey', 'xy_y_z_idx') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("ab"), T("-1")],
                            &[T("ab_b_c_idx"), T("0")],
                            &[T("ab_pkey"), T("0")],
                            &[T("xy"), T("-1")],
                            &[T("xy_pkey"), T("0")],
                            &[T("xy_y_z_idx"), T("0")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt gathers statistics in the background into its own store, read from dolt_statistics; Doltgrust keeps Postgres' statistics, gathered by ANALYZE into pg_class and pg_statistic")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE;",
                    expected: Expected::Tag("ANALYZE"),
                    dolt: Some(Dolt::Differs("Dolt gathers statistics in the background into its own store, read from dolt_statistics; Doltgrust keeps Postgres' statistics, gathered by ANALYZE into pg_class and pg_statistic")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltuples FROM pg_class WHERE relname IN ('ab', 'ab_b_c_idx', 'ab_pkey', 'xy', 'xy_pkey', 'xy_y_z_idx') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("ab"), T("6")],
                            &[T("ab_b_c_idx"), T("6")],
                            &[T("ab_pkey"), T("6")],
                            &[T("xy"), T("6")],
                            &[T("xy_pkey"), T("6")],
                            &[T("xy_y_z_idx"), T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt gathers statistics in the background into its own store, read from dolt_statistics; Doltgrust keeps Postgres' statistics, gathered by ANALYZE into pg_class and pg_statistic")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO xy VALUES ('6',3,'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE;",
                    expected: Expected::Tag("ANALYZE"),
                    dolt: Some(Dolt::Differs("Dolt gathers statistics in the background into its own store, read from dolt_statistics; Doltgrust keeps Postgres' statistics, gathered by ANALYZE into pg_class and pg_statistic")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltuples FROM pg_class WHERE relname IN ('ab', 'ab_b_c_idx', 'ab_pkey', 'xy', 'xy_pkey', 'xy_y_z_idx') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("ab"), T("6")],
                            &[T("ab_b_c_idx"), T("6")],
                            &[T("ab_pkey"), T("6")],
                            &[T("xy"), T("7")],
                            &[T("xy_pkey"), T("7")],
                            &[T("xy_y_z_idx"), T("7")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::Differs("Dolt gathers statistics in the background into its own store, read from dolt_statistics; Doltgrust keeps Postgres' statistics, gathered by ANALYZE into pg_class and pg_statistic")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltuples FROM pg_class WHERE relname IN ('ab', 'ab_b_c_idx', 'ab_pkey', 'xy', 'xy_pkey', 'xy_y_z_idx') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("xy"), T("6")],
                            &[T("xy_pkey"), T("6")],
                            &[T("xy_y_z_idx"), T("6")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt gathers statistics in the background into its own store, read from dolt_statistics; Doltgrust keeps Postgres' statistics, gathered by ANALYZE into pg_class and pg_statistic")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "issue #7710: branch connection string errors",
            intent: "ANALYZE works after switching to the database's main branch.",
            set_up_script: &[
                "CREATE TABLE xy (x bigint PRIMARY KEY, y int, z varchar(500));",
                "CREATE INDEX ON xy (y, z);",
                "INSERT INTO xy VALUES (0,0,'a'), (1,0,'a'), (2,0,'a'), (3,0,'a'), (4,1,'a'), (5,2,'a');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE xy;",
                    expected: Expected::Tag("ANALYZE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ANALYZE TABLE returns a status row, where Postgres' ANALYZE returns its tag")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
