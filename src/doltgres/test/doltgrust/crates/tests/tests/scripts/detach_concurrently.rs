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

use harness::oid::*;
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};


#[test]
fn test_detach_partition_concurrently() {
    run_scripts(&[
        ScriptTest {
            name: "DETACH PARTITION CONCURRENTLY waits for the transactions that could see the partition attached, which keep seeing it while new ones do not",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt SELECT i, 'x' FROM generate_series(0, 19) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 CONCURRENTLY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO pt VALUES (15, 'y');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "no partition of relation \"pt\" found for row", detail: "Partition key of the failing row contains (a) = (15).", schema: "public", table: "pt", ..E }),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT inhrelid::regclass FROM pg_inherits WHERE inhparent = 'pt'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("inhrelid", REGCLASS)],
                        rows: &[
                            &[T("pt1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT count(*) FROM pt2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DETACH PARTITION ... FINALIZE finishes a DETACH CONCURRENTLY that stopped waiting, once the older snapshots are gone",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt SELECT i, 'x' FROM generate_series(0, 19) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET lock_timeout = '200ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 CONCURRENTLY;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ RESET lock_timeout;",
                    expected: Expected::Tag("RESET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT inhrelid::regclass, inhdetachpending FROM pg_inherits WHERE inhparent = 'pt'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("inhrelid", REGCLASS), Column("inhdetachpending", BOOL)],
                        rows: &[
                            &[T("pt1"), T("f")],
                            &[T("pt2"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt1 CONCURRENTLY;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "partition \"pt2\" already pending detach in partitioned table \"public.pt\"", hint: "Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation.", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 FINALIZE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT inhrelid::regclass FROM pg_inherits WHERE inhparent = 'pt'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("inhrelid", REGCLASS)],
                        rows: &[
                            &[T("pt1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DETACH PARTITION ... FINALIZE returns at once with no other transactions open",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt SELECT i, 'x' FROM generate_series(0, 19) i;",
                "SET lock_timeout = '200ms';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET lock_timeout = '200ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 CONCURRENTLY;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 FINALIZE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a partition whose detach is pending is left out of its partitioned table's TRUNCATE and refuses ALTER TABLE",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt SELECT i, 'x' FROM generate_series(0, 19) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET lock_timeout = '200ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 CONCURRENTLY;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE pt;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pt1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pt2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt2 ALTER a DROP NOT NULL;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot alter partition \"pt2\" with an incomplete detach", hint: "Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt DETACH PARTITION pt2 FINALIZE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt2 ALTER a DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a REPEATABLE READ transaction whose snapshot predates a DETACH CONCURRENTLY's first commit truncates the partition through its partitioned table, and the pending detach survives it",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt SELECT i, 'x' FROM generate_series(0, 19) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE pt DETACH PARTITION pt2 CONCURRENTLY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ TRUNCATE pt;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pt2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT inhrelid::regclass FROM pg_inherits WHERE inhparent = 'pt'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("inhrelid", REGCLASS)],
                        rows: &[
                            &[T("pt1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pt2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
    ]);
}
