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

//! Dolt's integration-tests/bats/sql-multi-db.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_multi_db() {
    run_scripts(&[
        ScriptTest {
            name: r#"sql-multi-db: sql multi-db test show databases"#,
            intent: r#"A server lists each of its databases."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
                r#"CREATE DATABASE repo2;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname LIKE 'repo%' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(r#"repo1"#)],
                            &[T(r#"repo2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"SHOW DATABASES is MySQL's, so the port reads pg_database"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-multi-db: sql use statement and table accessibility"#,
            intent: r#"A database's own tables are read unqualified, and another database's through its name."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
                r#"CREATE DATABASE repo2;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r1_t1 (pk BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r1_t1 (pk) VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r2_t1 (pk BIGINT, c1 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_t1 (pk, c1) VALUES (2, 200), (3, 300), (4, 400);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r1_t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r2_t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T(r#"2"#), T(r#"200"#)],
                            &[T(r#"3"#), T(r#"300"#)],
                            &[T(r#"4"#), T(r#"400"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r2_t1;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "r2_t1" does not exist"#, position: 15, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r1_t1;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "r1_t1" does not exist"#, position: 15, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM repo2.public.r2_t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T(r#"2"#), T(r#"200"#)],
                            &[T(r#"3"#), T(r#"300"#)],
                            &[T(r#"4"#), T(r#"400"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo1",
                    database: "repo1",
                    skip: Some(r#"reading another database's table as db.schema.table (STORAGE_DESIGN section V.3) is designed but not built, failing with 0A000 cross-database references are not implemented"#),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's db.table names another database's table, which Doltgrust reads as db.schema.table"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM repo1.public.r1_t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    skip: Some(r#"reading another database's table as db.schema.table (STORAGE_DESIGN section V.3) is designed but not built, failing with 0A000 cross-database references are not implemented"#),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-multi-db: sql test use invalid db name"#,
            intent: r#"A connection to a database that doesn't exist fails."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
                r#"CREATE DATABASE repo2;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r1_t1 (pk BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r1_t1 (pk) VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r2_t1 (pk BIGINT, c1 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_t1 (pk, c1) VALUES (2, 200), (3, 300), (4, 400);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 1;"#,
                    expected: Expected::ClientError("database \"invalid_db_name\" does not exist"),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    client: "invalid",
                    database: "invalid_db_name",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-multi-db: sql join tables in different databases"#,
            intent: r#"A query joins its own table with another database's."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
                r#"CREATE DATABASE repo2;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r1_t1 (pk BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r1_t1 (pk) VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r2_t1 (pk BIGINT, c1 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_t1 (pk, c1) VALUES (2, 200), (3, 300), (4, 400);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT r1_t1.pk AS pk, r2.c1 AS c1 FROM r1_t1 JOIN repo2.public.r2_t1 r2 ON r1_t1.pk = r2.pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T(r#"2"#), T(r#"200"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    skip: Some(r#"reading another database's table as db.schema.table (STORAGE_DESIGN section V.3) is designed but not built, failing with 0A000 cross-database references are not implemented"#),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's db.table names another database's table, which Doltgrust reads as db.schema.table"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-multi-db: join on multiple databases with same name"#,
            intent: r#"Tables of one name in two databases join without ambiguity."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
                r#"CREATE DATABASE repo2;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r1_t1 (pk BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r1_t1 (pk) VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r2_t1 (pk BIGINT, c1 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_t1 (pk, c1) VALUES (2, 200), (3, 300), (4, 400);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r2_t1 (pk BIGINT, c1 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_t1 (pk, c1) VALUES (2, 200), (3, 300), (4, 400);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM repo1.public.r2_t1 JOIN repo2.public.r2_t1 ON repo1.public.r2_t1.pk = repo2.public.r2_t1.pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    skip: Some(r#"reading another database's table as db.schema.table (STORAGE_DESIGN section V.3) is designed but not built, failing with 0A000 cross-database references are not implemented"#),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's db.table names another database's table, which Doltgrust reads as db.schema.table"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-multi-db: fetch multiple databases with appropriate tempdir"#,
            intent: r#"A clone fetches from its remote on a server of several databases."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
                r#"CREATE DATABASE repo2;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote1}', 'cloned');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "cloned",
                    database: "cloned",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
