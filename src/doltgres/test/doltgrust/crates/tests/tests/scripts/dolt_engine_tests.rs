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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_engine_tests.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_run_add_drop_primary_keys_tests() {
    run_scripts(&[
        ScriptTest {
            name: r#"add primary keys"#,
            intent: r#"Adding and dropping a primary key repeatedly leaves one not-null constraint on the column."#,
            set_up_script: &[
                r#"CREATE TABLE test (id INT NOT NULL, c1 INT);"#,
                r#"CREATE INDEX c1_idx ON test (c1);"#,
                r#"INSERT INTO test VALUES (1, 1), (2, 2);"#,
                r#"ALTER TABLE test ADD PRIMARY KEY (id);"#,
                r#"ALTER TABLE test DROP CONSTRAINT test_pkey;"#,
                r#"ALTER TABLE test ADD PRIMARY KEY (id);"#,
                r#"ALTER TABLE test DROP CONSTRAINT test_pkey;"#,
                r#"ALTER TABLE test ADD PRIMARY KEY (id);"#,
                r#"ALTER TABLE test DROP CONSTRAINT test_pkey;"#,
                r#"ALTER TABLE test ADD PRIMARY KEY (id);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT conname, contype FROM pg_constraint WHERE conrelid = 'test'::regclass ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("contype", CHAR)],
                        rows: &[
                            &[T(r#"test_id_not_null"#), T(r#"n"#)],
                            &[T(r#"test_pkey"#), T(r#"p"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"SHOW CREATE TABLE is MySQL's, so the port reads the table's constraints and indexes from the catalogs"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T(r#"c1_idx"#)],
                            &[T(r#"test_pkey"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"add primary keys to table with index"#,
            intent: r#"Adding a primary key to a table with an index and a check keeps both and the index's entries."#,
            set_up_script: &[
                r#"CREATE TABLE test (id INT NOT NULL, c1 INT);"#,
                r#"CREATE INDEX c1_idx ON test (c1);"#,
                r#"INSERT INTO test VALUES (1, 1), (2, 2);"#,
                r#"ALTER TABLE test ADD CONSTRAINT test_check CHECK (c1 > 0);"#,
                r#"ALTER TABLE test ADD PRIMARY KEY (id);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT conname, contype FROM pg_constraint WHERE conrelid = 'test'::regclass ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("contype", CHAR)],
                        rows: &[
                            &[T(r#"test_check"#), T(r#"c"#)],
                            &[T(r#"test_id_not_null"#), T(r#"n"#)],
                            &[T(r#"test_pkey"#), T(r#"p"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"SHOW CREATE TABLE is MySQL's, so the port reads the table's constraints and indexes from the catalogs"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T(r#"c1_idx"#)],
                            &[T(r#"test_pkey"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                            &[T(r#"2"#), T(r#"2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the Go test counts the index's entries, so the port reads c1_idx through an index scan"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test WHERE c1 > 0;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"Add primary key when one more cells contain NULL"#,
            intent: r#"A primary key over a column holding NULLs fails."#,
            set_up_script: &[
                r#"CREATE TABLE test (id INT NOT NULL, c1 INT);"#,
                r#"CREATE INDEX c1_idx ON test (c1);"#,
                r#"INSERT INTO test VALUES (1, 1), (2, 2);"#,
                r#"ALTER TABLE test ADD PRIMARY KEY (c1);"#,
                r#"ALTER TABLE test ADD COLUMN c2 INT NULL;"#,
                r#"ALTER TABLE test DROP CONSTRAINT test_pkey;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test ADD PRIMARY KEY (id, c1, c2);"#,
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"column "c2" of relation "test" contains null values"#, schema: "public", table: "test", column: "c2", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"Drop primary key from table with index"#,
            intent: r#"Dropping a primary key keeps the table's other index and its declared not-null constraint."#,
            set_up_script: &[
                r#"CREATE TABLE test (id INT NOT NULL PRIMARY KEY, c1 INT);"#,
                r#"CREATE INDEX c1_idx ON test (c1);"#,
                r#"INSERT INTO test VALUES (1, 1), (2, 2);"#,
                r#"ALTER TABLE test DROP CONSTRAINT test_pkey;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT conname, contype FROM pg_constraint WHERE conrelid = 'test'::regclass ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("contype", CHAR)],
                        rows: &[
                            &[T(r#"test_id_not_null"#), T(r#"n"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"SHOW CREATE TABLE is MySQL's, so the port reads the table's constraints and indexes from the catalogs"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T(r#"c1_idx"#)],
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

#[test]
fn test_run_drop_engine_test() {
    run_scripts(&[
        ScriptTest {
            name: r#"Drop database engine tests for Dolt only"#,
            intent: r#"Unquoted database names fold to lower case, so a drop in any case finds the database and connections to it fail afterward."#,
            set_up_script: &[
                r#"CREATE DATABASE Test1db;"#,
                r#"CREATE DATABASE TEST2db;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP DATABASE TeSt2DB;"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 1;"#,
                    expected: Expected::ClientError("database \"test2db\" does not exist"),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, whose name it matches exactly"#)),
                    client: "t2",
                    database: "test2db",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT current_database();"#,
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T(r#"test1db"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "t1",
                    database: "test1db",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE IF EXISTS test1DB WITH (FORCE);"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    dolt: Some(Dolt::MySqlOnly(r#"Postgres refuses to drop a database other sessions are connected to without FORCE"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 1;"#,
                    expected: Expected::ClientError("database \"test1db\" does not exist"),
                    client: "t1again",
                    database: "test1db",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_run_dolt_revision_db_scripts_test() {
    run_scripts(&[
        ScriptTest {
            name: r#"database revision specs: commit-qualified revision spec"#,
            intent: r#"A connection to a commit reads it read-only, refuses a reset, and moves to a branch with dolt_checkout."#,
            set_up_script: &[
                r#"CREATE TABLE t01 (pk INT PRIMARY KEY, c1 INT);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'creating table t01 on main');"#,
                r#"INSERT INTO t01 VALUES (1, 1), (2, 2);"#,
                r#"SELECT dolt_commit('-am', 'adding rows to table t01 on main');"#,
                r#"INSERT INTO t01 VALUES (3, 3);"#,
                r#"SELECT dolt_commit('-am', 'adding another row to table t01 on main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"the test names the commit by its hash, and the port by main~2, the same commit"#)),
                    client: "rev",
                    database: "postgres/main~2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT current_database();"#,
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T(r#"postgres"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs(r#"a revision connection's current_database() is the database's own name (Appendix C, revision connections), where Dolt's database() returns mydb/<hash>"#)),
                    client: "rev",
                    database: "postgres/main~2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t01;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "rev",
                    database: "postgres/main~2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset();"#,
                    expected: Expected::Error(Diagnostic { code: "25006", message: r#"cannot execute dolt_reset() in a read-only transaction"#, ..E }),
                    dolt: Some(Dolt::Differs(r#"a revision connection refuses writes as Postgres refuses them in a read-only transaction, where Dolt says unable to reset HEAD in read-only databases"#)),
                    client: "rev",
                    database: "postgres/main~2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "rev",
                    database: "postgres/main~2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT current_database();"#,
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T(r#"postgres"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "rev",
                    database: "postgres/main~2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "rev",
                    database: "postgres/main~2",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
