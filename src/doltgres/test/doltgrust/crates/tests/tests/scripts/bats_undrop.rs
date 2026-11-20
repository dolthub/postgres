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

//! Dolt's integration-tests/bats/undrop.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_undrop() {
    run_scripts(&[
        ScriptTest {
            name: r#"undrop: undrop error messages"#,
            intent: r#"dolt_undrop without a name, with a name no dropped database has, or with several names fails, naming the databases it could restore."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no database name specified. there are no databases currently available to be undropped"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop('doesnotexist');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no database named 'doesnotexist' found to undrop. there are no databases currently available to be undropped"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop('one', 'two', 'three');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"dolt_undrop called with too many arguments: dolt_undrop only accepts one argument - the name of the dropped database to restore"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE dropper;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE dropper;"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no database name specified. available databases that can be undropped: dropper"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"undrop: purge error messages"#,
            intent: r#"dolt_purge_dropped_databases takes no arguments."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_purge_dropped_databases('all', 'of', 'the', 'dbs');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"dolt_purge_dropped_databases does not take any arguments"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"undrop: undrop non-root database with hyphen replaced in its name"#,
            intent: r#"A dropped database whose name has only word characters comes back with its tables under the same name."#,
            set_up_script: &[
                r#"CREATE DATABASE drop_me_2;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (pk int PRIMARY KEY, c1 varchar(200));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "d",
                    database: "drop_me_2",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 'one');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "d",
                    database: "drop_me_2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'creating table t1');"#,
                    expected: Expected::Ok,
                    client: "d",
                    database: "drop_me_2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "d",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("drop_me_2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE drop_me_2;"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop('drop_me_2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_undrop", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres' database names are case-sensitive, so the port restores the database by its exact name, where Dolt's test passes another case")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("drop_me_2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "e",
                    database: "drop_me_2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"undrop: undrop non-root database with hyphen allowed in its name"#,
            intent: r#"A dropped database whose name has spaces and hyphens comes back with its tables under the same name."#,
            set_up_script: &[
                r#"CREATE DATABASE " drop- me-2 ";"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (pk int PRIMARY KEY, c1 varchar(200));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "d",
                    database: " drop- me-2 ",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 'one');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "d",
                    database: " drop- me-2 ",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'creating table t1');"#,
                    expected: Expected::Ok,
                    client: "d",
                    database: " drop- me-2 ",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "d",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(" drop- me-2 ")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE " drop- me-2 ";"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop(' drop- me-2 ');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_undrop", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres' database names are case-sensitive, so the port restores the database by its exact name, where Dolt's test passes another case")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(" drop- me-2 ")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "e",
                    database: " drop- me-2 ",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"undrop: drop database, recreate, and drop again"#,
            intent: r#"After two databases of one name are dropped, dolt_undrop restores the newer."#,
            set_up_script: &[
                r#"CREATE DATABASE test123;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (pk int PRIMARY KEY, c1 varchar(200));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    database: "test123",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 'one');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    database: "test123",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'adding table t1 to test123 database');"#,
                    expected: Expected::Ok,
                    client: "a",
                    database: "test123",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "a",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE test123;"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test123;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t2 (pk int PRIMARY KEY, c1 varchar(200));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "b",
                    database: "test123",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 VALUES (100, 'one hundy');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    database: "test123",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'adding table t2 to new test123 database');"#,
                    expected: Expected::Ok,
                    client: "b",
                    database: "test123",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "b",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE test123;"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop('test123');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_undrop", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("100"), T("one hundy")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c",
                    database: "test123",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"undrop: undrop conflict"#,
            intent: r#"A dropped database can't be restored while another database has its name."#,
            set_up_script: &[
                r#"CREATE DATABASE database1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (pk int PRIMARY KEY, c1 varchar(200));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    database: "database1",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 'one');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    database: "database1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'creating table t1');"#,
                    expected: Expected::Ok,
                    client: "a",
                    database: "database1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "a",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("database1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE database1;"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE database1;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t2 (pk int PRIMARY KEY, c1 varchar(200));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "b",
                    database: "database1",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 VALUES (1000, 'thousand');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    database: "database1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'creating table t2');"#,
                    expected: Expected::Ok,
                    client: "b",
                    database: "database1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "b",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("database1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop('database1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unable to undrop database 'database1'; another database already exists with the same name"#, ..E }),
                    dolt: Some(Dolt::Differs("Postgres' database names are case-sensitive, so the clash is with the exact name, where Dolt's message says the same case-insensitive name and its test differs only in case")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"undrop: purging dropped databases"#,
            intent: r#"Purging removes the dropped databases for good and keeps the others."#,
            set_up_script: &[
                r#"CREATE DATABASE keepme;"#,
                r#"CREATE DATABASE purgeme;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t3 (pk int PRIMARY KEY, c1 varchar(200));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    database: "purgeme",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t3 VALUES (3, 'three');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    database: "purgeme",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'creating table t3');"#,
                    expected: Expected::Ok,
                    client: "a",
                    database: "purgeme",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "a",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("keepme")],
                            &[T("purgeme")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_purge_dropped_databases();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_purge_dropped_databases", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE purgeme;"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("keepme")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_purge_dropped_databases();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_purge_dropped_databases", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_undrop('purgeme');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no database named 'purgeme' found to undrop. there are no databases currently available to be undropped"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("keepme")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, so the port lists pg_database")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
