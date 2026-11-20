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

//! Dolt's go/libraries/doltcore/sqle/enginetest/branch_activity_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_branch_activity_tests() {
    run_scripts(&[
        ScriptTest {
            name: "branch activity table shows all branches",
            intent: "dolt_branch_activity lists every branch.",
            set_up_script: &[
                "SELECT dolt_branch('feature1');",
                "SELECT dolt_branch('feature2');",
                "SELECT pg_sleep(0.5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_branch_activity;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch FROM dolt_branch_activity ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("feature1")],
                            &[T("feature2")],
                            &[T("main")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            ..S
        },
        ScriptTest {
            name: "branch creation does sets last write but not last read",
            intent: "A new branch has a last write time and no last read time.",
            set_up_script: &[
                "SELECT dolt_branch('new_branch');",
                "SELECT pg_sleep(0.5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT branch, last_read , last_write IS NULL FROM dolt_branch_activity WHERE branch = 'new_branch';",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("last_read", TIMESTAMPTZ), Column("?column?", BOOL)],
                        rows: &[
                            &[T("new_branch"), Null, T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            ..S
        },
        ScriptTest {
            name: "AS OF updates the read time for a branch",
            intent: "Reading a branch with AS OF sets its last read time, and AS OF 'HEAD' adds no row for HEAD.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2), (3);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('new_branch');",
                "SELECT pg_sleep(0.5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT branch, last_read IS NULL, last_write IS NULL FROM dolt_branch_activity WHERE branch = 'new_branch';",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("new_branch"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD';",
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
                    query: "SELECT * FROM t AS OF 'new_branch';",
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
                    query: "SELECT pg_sleep(0.5);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch, last_read IS NULL, last_write IS NULL FROM dolt_branch_activity WHERE branch = 'new_branch';",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("new_branch"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch, last_read IS NULL, last_write IS NULL FROM dolt_branch_activity WHERE branch = 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            ..S
        },
        ScriptTest {
            name: "branch checkout counts as read activity",
            intent: "Checking a branch out sets its last read time after its last write time.",
            set_up_script: &[
                "SELECT dolt_branch('test_branch');",
                "SELECT pg_sleep(4);",
                "SELECT * FROM dolt_checkout('test_branch');",
                "SELECT pg_sleep(0.5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT branch, last_read IS NOT NULL, last_write IS NOT NULL FROM dolt_branch_activity WHERE branch = 'test_branch';",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("test_branch"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch, floor(extract(epoch FROM (last_read)::timestamp - (last_write)::timestamp))::bigint > 3, floor(extract(epoch FROM (last_read)::timestamp - (last_write)::timestamp))::bigint <=5 FROM dolt_branch_activity WHERE branch = 'test_branch';",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("test_branch"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_branch_activity WHERE system_start_time IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            ..S
        },
        ScriptTest {
            name: "branch activity filtered for delete branches",
            intent: "A deleted branch leaves dolt_branch_activity.",
            set_up_script: &[
                "SELECT dolt_branch('temp_branch');",
                "SELECT * FROM dolt_checkout('temp_branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT dolt_branch('-d', 'temp_branch');",
                "SELECT pg_sleep(0.5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_branch_activity WHERE branch = 'temp_branch';",
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
                    query: "SELECT branch FROM dolt_branch_activity;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            ..S
        },
        ScriptTest {
            name: "branch activity when writing to another branch",
            intent: "An update through another branch's revision name moves that branch's last write time.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(20));",
                "INSERT INTO t VALUES (1,'foo');",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('other_branch');",
                "SELECT pg_sleep(0.5);",
                "SELECT set_config('test.lw', (last_write)::text, FALSE) FROM dolt_branch_activity WHERE branch = 'other_branch';",
                "SELECT pg_sleep(2);",
                "UPDATE \"postgres/other_branch\".public.t SET v='baz' WHERE id=1;",
                "SELECT pg_sleep(0.5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT (last_write)::text <> current_setting('test.lw', TRUE) FROM dolt_branch_activity WHERE branch = 'other_branch';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's SELECT ... INTO @lw sets the setting standing for @lw, which holds text")),
                    ..A
                },
            ],
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            ..S
        },
    ]);
}

#[test]
fn test_branch_activity_of_prepared_transactions() {
    run_scripts(&[
        ScriptTest {
            name: "a prepared transaction's write moves its branch's last write time when COMMIT PREPARED commits it",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'made t');",
                "SELECT dolt_branch('other');",
                "SELECT set_config('test.lw', (last_write)::text, FALSE) FROM dolt_branch_activity WHERE branch = 'other';",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", client: "o", database: "postgres/other", ..A },
                ScriptTestAssertion { query: "INSERT INTO t VALUES (1);", client: "o", database: "postgres/other", ..A },
                ScriptTestAssertion { query: "PREPARE TRANSACTION 'p';", client: "o", database: "postgres/other", ..A },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT (last_write)::text = current_setting('test.lw') AS unchanged FROM dolt_branch_activity WHERE branch = 'other';",
                    expected: Expected::Rows {
                        columns: &[Column("unchanged", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "COMMIT PREPARED 'p';", ..A },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT last_write > (current_setting('test.lw'))::timestamptz AS moved FROM dolt_branch_activity WHERE branch = 'other';",
                    expected: Expected::Rows {
                        columns: &[Column("moved", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            server_config: "behavior:\n  branch_activity_tracking: true\nsystem_variables:\n  max_prepared_transactions: 2\n",
            ..S
        },
    ]);
}
