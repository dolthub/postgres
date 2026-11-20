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

//! The `patch` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_patch() {
    run_scripts(&[
        ScriptTest {
            name: r#"basic_insert_update_delete"#,
            intent: r#"Doltlite oracle case: basic insert update delete."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e7453455463313d27756e6f272c6e3d31315748455245706b3d313b"#)],
                            &[T(r#"P|2|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d323b"#)],
                            &[T(r#"P|3|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c6e2956414c55455328332c277468726565272c3330293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"reverse_insert_update_delete"#,
            intent: r#"Doltlite oracle case: reverse insert update delete."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD','HEAD~1') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e7453455463313d276f6e65272c6e3d31305748455245706b3d313b"#)],
                            &[T(r#"P|2|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c6e2956414c55455328322c2774776f272c3230293b"#)],
                            &[T(r#"P|3|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d333b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"two_dot_range"#,
            intent: r#"Doltlite oracle case: two dot range."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1..HEAD') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e7453455463313d27756e6f272c6e3d31315748455245706b3d313b"#)],
                            &[T(r#"P|2|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d323b"#)],
                            &[T(r#"P|3|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c6e2956414c55455328332c277468726565272c3330293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"range_with_table"#,
            intent: r#"Doltlite oracle case: range with table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1..HEAD','t') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e7453455463313d27756e6f272c6e3d31315748455245706b3d313b"#)],
                            &[T(r#"P|2|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d323b"#)],
                            &[T(r#"P|3|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c6e2956414c55455328332c277468726565272c3330293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"explicit_table"#,
            intent: r#"Doltlite oracle case: explicit table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1','HEAD','t') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e7453455463313d27756e6f272c6e3d31315748455245706b3d313b"#)],
                            &[T(r#"P|2|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d323b"#)],
                            &[T(r#"P|3|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c6e2956414c55455328332c277468726565272c3330293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case_insensitive_table"#,
            intent: r#"Doltlite oracle case: case insensitive table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1','HEAD','T') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e7453455463313d27756e6f272c6e3d31315748455245706b3d313b"#)],
                            &[T(r#"P|2|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d323b"#)],
                            &[T(r#"P|3|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c6e2956414c55455328332c277468726565272c3330293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"data_filter_count"#,
            intent: r#"Doltlite oracle case: data filter count."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',count(*))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"schema_filter_count"#,
            intent: r#"Doltlite oracle case: schema filter count."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',count(*))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='schema';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"unknown_filter_empty"#,
            intent: r#"Doltlite oracle case: unknown filter empty."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, n integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one',10),(2,'two',20),(4,NULL,40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c1='uno',n=11 WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3,'three',30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',count(*))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='unknown';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"table_rename_single_schema_statement"#,
            intent: r#"Doltlite oracle case: table rename single schema statement."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t RENAME TO u;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','target');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',count(*))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='schema';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"table_rename_has_no_data_statements"#,
            intent: r#"Doltlite oracle case: table rename has no data statements."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'one'),(2,'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t RENAME TO u;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','target');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',count(*))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"multi_table_order"#,
            intent: r#"Doltlite oracle case: multi table order."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE z(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO z VALUES (1,'z');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE z SET v='zee' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE a SET v='aye' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.a|data|5550444154457075626c69632e61534554763d27617965275748455245706b3d313b"#)],
                            &[T(r#"P|2|public.z|data|5550444154457075626c69632e7a534554763d277a6565275748455245706b3d313b"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"added_table_rows"#,
            intent: r#"Doltlite oracle case: added table rows."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE seed(pk integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE added(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO added VALUES (1,'a'),(2,'b');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|4|public.added|data|494e53455254494e544f7075626c69632e616464656428706b2c762956414c55455328312c276127293b"#)],
                            &[T(r#"P|5|public.added|data|494e53455254494e544f7075626c69632e616464656428706b2c762956414c55455328322c276227293b"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"dropped_table_has_no_data"#,
            intent: r#"Doltlite oracle case: dropped table has no data."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE gone(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO gone VALUES (1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE gone;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',table_name,diff_type)) FROM dolt_patch('HEAD~1','HEAD') WHERE 1;"#,
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"argument of WHERE must be type boolean, not type integer"#, position: 96, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"composite_primary_key"#,
            intent: r#"Doltlite oracle case: composite primary key."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(a VARCHAR(20), b integer, v TEXT, PRIMARY KEY(a,b));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('x',1,'old'),('x',2,'delete');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='new' WHERE a='x' AND b=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE a='x' AND b=2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('y',3,'add');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e74534554763d276e6577275748455245613d277827414e44623d313b"#)],
                            &[T(r#"P|2|public.t|data|44454c45544546524f4d7075626c69632e745748455245613d277827414e44623d323b"#)],
                            &[T(r#"P|3|public.t|data|494e53455254494e544f7075626c69632e7428612c622c762956414c554553282779272c332c2761646427293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"working_label"#,
            intent: r#"Doltlite oracle case: working label."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='working' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',table_name,diff_type,to_commit_hash)) FROM dolt_patch('HEAD','WORKING') WHERE 1;"#,
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"argument of WHERE must be type boolean, not type integer"#, position: 112, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"staged_label"#,
            intent: r#"Doltlite oracle case: staged label."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='staged' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_add('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',table_name,diff_type,to_commit_hash)) FROM dolt_patch('HEAD','STAGED') WHERE 1;"#,
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"argument of WHERE must be type boolean, not type integer"#, position: 111, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"three_dot_merge_base"#,
            intent: r#"Doltlite oracle case: three dot merge base."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'base');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2,'feature');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='main' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('feature...main') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e74534554763d276d61696e275748455245706b3d313b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"branch_explicit_main_to_branch"#,
            intent: r#"Doltlite oracle case: branch explicit main to branch."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, c2 TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'one','two');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'main','row');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE newtable(pk integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO newtable VALUES(1),(2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main table');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'branch1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN c2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(3,'branch');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','branch change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('main','branch1') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|3|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d313b"#)],
                            &[T(r#"P|4|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d323b"#)],
                            &[T(r#"P|5|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312956414c55455328332c276272616e636827293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"branch_two_dot_main_to_branch"#,
            intent: r#"Doltlite oracle case: branch two dot main to branch."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, c2 TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'one','two');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'main','row');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE newtable(pk integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO newtable VALUES(1),(2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main table');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'branch1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN c2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(3,'branch');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','branch change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('main..branch1') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|3|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d313b"#)],
                            &[T(r#"P|4|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d323b"#)],
                            &[T(r#"P|5|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312956414c55455328332c276272616e636827293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"branch_explicit_reverse"#,
            intent: r#"Doltlite oracle case: branch explicit reverse."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, c2 TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'one','two');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'main','row');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE newtable(pk integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO newtable VALUES(1),(2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main table');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'branch1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN c2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(3,'branch');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','branch change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('branch1','main') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|4|public.newtable|data|494e53455254494e544f7075626c69632e6e65777461626c6528706b2956414c5545532831293b"#)],
                            &[T(r#"P|5|public.newtable|data|494e53455254494e544f7075626c69632e6e65777461626c6528706b2956414c5545532832293b"#)],
                            &[T(r#"P|7|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c63322956414c55455328312c276f6e65272c2774776f27293b"#)],
                            &[T(r#"P|8|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c63322956414c55455328322c276d61696e272c27726f7727293b"#)],
                            &[T(r#"P|9|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d333b"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"branch_two_dot_reverse"#,
            intent: r#"Doltlite oracle case: branch two dot reverse."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, c2 TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'one','two');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'main','row');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE newtable(pk integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO newtable VALUES(1),(2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main table');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'branch1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN c2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(3,'branch');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','branch change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('branch1..main') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|4|public.newtable|data|494e53455254494e544f7075626c69632e6e65777461626c6528706b2956414c5545532831293b"#)],
                            &[T(r#"P|5|public.newtable|data|494e53455254494e544f7075626c69632e6e65777461626c6528706b2956414c5545532832293b"#)],
                            &[T(r#"P|7|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c63322956414c55455328312c276f6e65272c2774776f27293b"#)],
                            &[T(r#"P|8|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c63322956414c55455328322c276d61696e272c27726f7727293b"#)],
                            &[T(r#"P|9|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d333b"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"branch_three_dot_to_branch"#,
            intent: r#"Doltlite oracle case: branch three dot to branch."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, c2 TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'one','two');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'main','row');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE newtable(pk integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO newtable VALUES(1),(2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main table');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'branch1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN c2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(3,'branch');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','branch change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('main...branch1') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|2|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d313b"#)],
                            &[T(r#"P|3|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312956414c55455328332c276272616e636827293b"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"branch_three_dot_to_main"#,
            intent: r#"Doltlite oracle case: branch three dot to main."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, c1 TEXT, c2 TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'one','two');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'main','row');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE newtable(pk integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO newtable VALUES(1),(2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','main table');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'branch1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN c2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk=1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(3,'branch');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','branch change');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('branch1...main') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|4|public.newtable|data|494e53455254494e544f7075626c69632e6e65777461626c6528706b2956414c5545532831293b"#)],
                            &[T(r#"P|5|public.newtable|data|494e53455254494e544f7075626c69632e6e65777461626c6528706b2956414c5545532832293b"#)],
                            &[T(r#"P|6|public.t|data|494e53455254494e544f7075626c69632e7428706b2c63312c63322956414c55455328322c276d61696e272c27726f7727293b"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rename_filter_new_name"#,
            intent: r#"Doltlite oracle case: rename filter new name."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1(a integer PRIMARY KEY, b integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES(1,2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t1 RENAME TO t2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 VALUES(3,4);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','renamed');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1','HEAD','t2') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|2|public.t2|data|494e53455254494e544f7075626c69632e743228612c622956414c55455328332c34293b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rename_filter_old_name"#,
            intent: r#"Doltlite oracle case: rename filter old name."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1(a integer PRIMARY KEY, b integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES(1,2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t1 RENAME TO t2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 VALUES(3,4);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','renamed');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1..HEAD','t1') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|2|public.t2|data|494e53455254494e544f7075626c69632e743228612c622956414c55455328332c34293b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"head_to_staged"#,
            intent: r#"Doltlite oracle case: head to staged."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'base');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='staged' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_add('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='working' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD','STAGED') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e74534554763d27737461676564275748455245706b3d313b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"staged_to_working"#,
            intent: r#"Doltlite oracle case: staged to working."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'base');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='staged' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_add('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='working' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('STAGED','WORKING') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e74534554763d27776f726b696e67275748455245706b3d313b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"working_to_staged"#,
            intent: r#"Doltlite oracle case: working to staged."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'base');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='staged' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_add('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='working' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('WORKING','STAGED') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|5550444154457075626c69632e74534554763d27737461676564275748455245706b3d313b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"working_to_working_empty"#,
            intent: r#"Doltlite oracle case: working to working empty."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'base');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='staged' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_add('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='working' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',count(*))) FROM dolt_patch('WORKING','WORKING') WHERE 1;"#,
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"argument of WHERE must be type boolean, not type integer"#, position: 88, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"staged_to_staged_empty"#,
            intent: r#"Doltlite oracle case: staged to staged empty."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'base');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='staged' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_add('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='working' WHERE pk=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',concat_ws('|',count(*))) FROM dolt_patch('STAGED..STAGED') WHERE 1;"#,
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"argument of WHERE must be type boolean, not type integer"#, position: 85, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"multi_chunk_ordering"#,
            intent: r#"Doltlite oracle case: multi chunk ordering."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,'value1'),(2,'value2'),(3,'value3'),(4,'value4'),(5,'value5'),(6,'value6'),(7,'value7'),(8,'value8'),(9,'value9'),(10,'value10'),(11,'value11'),(12,'value12'),(13,'value13'),(14,'value14'),(15,'value15'),(16,'value16'),(17,'value17'),(18,'value18'),(19,'value19'),(20,'value20'),(21,'value21'),(22,'value22'),(23,'value23'),(24,'value24'),(25,'value25'),(26,'value26'),(27,'value27'),(28,'value28'),(29,'value29'),(30,'value30'),(31,'value31'),(32,'value32'),(33,'value33'),(34,'value34'),(35,'value35'),(36,'value36'),(37,'value37'),(38,'value38'),(39,'value39'),(40,'value40'),(41,'value41'),(42,'value42'),(43,'value43'),(44,'value44'),(45,'value45'),(46,'value46'),(47,'value47'),(48,'value48'),(49,'value49'),(50,'value50'),(51,'value51'),(52,'value52'),(53,'value53'),(54,'value54'),(55,'value55'),(56,'value56'),(57,'value57'),(58,'value58'),(59,'value59'),(60,'value60'),(61,'value61'),(62,'value62'),(63,'value63'),(64,'value64'),(65,'value65'),(66,'value66'),(67,'value67'),(68,'value68'),(69,'value69'),(70,'value70'),(71,'value71'),(72,'value72'),(73,'value73'),(74,'value74'),(75,'value75'),(76,'value76'),(77,'value77'),(78,'value78'),(79,'value79'),(80,'value80'),(81,'value81'),(82,'value82'),(83,'value83'),(84,'value84'),(85,'value85'),(86,'value86'),(87,'value87'),(88,'value88'),(89,'value89'),(90,'value90'),(91,'value91'),(92,'value92'),(93,'value93'),(94,'value94'),(95,'value95'),(96,'value96'),(97,'value97'),(98,'value98'),(99,'value99'),(100,'value100'),(101,'value101'),(102,'value102'),(103,'value103'),(104,'value104'),(105,'value105'),(106,'value106'),(107,'value107'),(108,'value108'),(109,'value109'),(110,'value110'),(111,'value111'),(112,'value112'),(113,'value113'),(114,'value114'),(115,'value115'),(116,'value116'),(117,'value117'),(118,'value118'),(119,'value119'),(120,'value120'),(121,'value121'),(122,'value122'),(123,'value123'),(124,'value124'),(125,'value125'),(126,'value126'),(127,'value127'),(128,'value128'),(129,'value129'),(130,'value130'),(131,'value131'),(132,'value132'),(133,'value133'),(134,'value134'),(135,'value135'),(136,'value136'),(137,'value137'),(138,'value138'),(139,'value139'),(140,'value140'),(141,'value141'),(142,'value142'),(143,'value143'),(144,'value144'),(145,'value145'),(146,'value146'),(147,'value147'),(148,'value148'),(149,'value149'),(150,'value150'),(151,'value151'),(152,'value152'),(153,'value153'),(154,'value154'),(155,'value155'),(156,'value156'),(157,'value157'),(158,'value158'),(159,'value159'),(160,'value160'),(161,'value161'),(162,'value162'),(163,'value163'),(164,'value164'),(165,'value165'),(166,'value166'),(167,'value167'),(168,'value168'),(169,'value169'),(170,'value170'),(171,'value171'),(172,'value172'),(173,'value173'),(174,'value174'),(175,'value175'),(176,'value176'),(177,'value177'),(178,'value178'),(179,'value179'),(180,'value180'),(181,'value181'),(182,'value182'),(183,'value183'),(184,'value184'),(185,'value185'),(186,'value186'),(187,'value187'),(188,'value188'),(189,'value189'),(190,'value190'),(191,'value191'),(192,'value192'),(193,'value193'),(194,'value194'),(195,'value195'),(196,'value196'),(197,'value197'),(198,'value198'),(199,'value199'),(200,'value200'),(201,'value201'),(202,'value202'),(203,'value203'),(204,'value204'),(205,'value205'),(206,'value206'),(207,'value207'),(208,'value208'),(209,'value209'),(210,'value210'),(211,'value211'),(212,'value212'),(213,'value213'),(214,'value214'),(215,'value215'),(216,'value216'),(217,'value217'),(218,'value218'),(219,'value219'),(220,'value220'),(221,'value221'),(222,'value222'),(223,'value223'),(224,'value224'),(225,'value225'),(226,'value226'),(227,'value227'),(228,'value228'),(229,'value229'),(230,'value230'),(231,'value231'),(232,'value232'),(233,'value233'),(234,'value234'),(235,'value235'),(236,'value236'),(237,'value237'),(238,'value238'),(239,'value239'),(240,'value240'),(241,'value241'),(242,'value242'),(243,'value243'),(244,'value244'),(245,'value245'),(246,'value246'),(247,'value247'),(248,'value248'),(249,'value249'),(250,'value250'),(251,'value251'),(252,'value252'),(253,'value253'),(254,'value254'),(255,'value255'),(256,'value256'),(257,'value257'),(258,'value258'),(259,'value259'),(260,'value260'),(261,'value261'),(262,'value262'),(263,'value263'),(264,'value264'),(265,'value265'),(266,'value266'),(267,'value267'),(268,'value268'),(269,'value269'),(270,'value270'),(271,'value271'),(272,'value272'),(273,'value273'),(274,'value274'),(275,'value275'),(276,'value276'),(277,'value277'),(278,'value278'),(279,'value279'),(280,'value280'),(281,'value281'),(282,'value282'),(283,'value283'),(284,'value284'),(285,'value285'),(286,'value286'),(287,'value287'),(288,'value288'),(289,'value289'),(290,'value290'),(291,'value291'),(292,'value292'),(293,'value293'),(294,'value294'),(295,'value295'),(296,'value296'),(297,'value297'),(298,'value298'),(299,'value299'),(300,'value300'),(301,'value301'),(302,'value302'),(303,'value303'),(304,'value304'),(305,'value305'),(306,'value306'),(307,'value307'),(308,'value308'),(309,'value309'),(310,'value310'),(311,'value311'),(312,'value312'),(313,'value313'),(314,'value314'),(315,'value315'),(316,'value316'),(317,'value317'),(318,'value318'),(319,'value319'),(320,'value320'),(321,'value321'),(322,'value322'),(323,'value323'),(324,'value324'),(325,'value325'),(326,'value326'),(327,'value327'),(328,'value328'),(329,'value329'),(330,'value330'),(331,'value331'),(332,'value332'),(333,'value333'),(334,'value334'),(335,'value335'),(336,'value336'),(337,'value337'),(338,'value338'),(339,'value339'),(340,'value340'),(341,'value341'),(342,'value342'),(343,'value343'),(344,'value344'),(345,'value345'),(346,'value346'),(347,'value347'),(348,'value348'),(349,'value349'),(350,'value350'),(351,'value351'),(352,'value352'),(353,'value353'),(354,'value354'),(355,'value355'),(356,'value356'),(357,'value357'),(358,'value358'),(359,'value359'),(360,'value360'),(361,'value361'),(362,'value362'),(363,'value363'),(364,'value364'),(365,'value365'),(366,'value366'),(367,'value367'),(368,'value368'),(369,'value369'),(370,'value370'),(371,'value371'),(372,'value372'),(373,'value373'),(374,'value374'),(375,'value375'),(376,'value376'),(377,'value377'),(378,'value378'),(379,'value379'),(380,'value380'),(381,'value381'),(382,'value382'),(383,'value383'),(384,'value384'),(385,'value385'),(386,'value386'),(387,'value387'),(388,'value388'),(389,'value389'),(390,'value390'),(391,'value391'),(392,'value392'),(393,'value393'),(394,'value394'),(395,'value395'),(396,'value396'),(397,'value397'),(398,'value398'),(399,'value399'),(400,'value400');"#,
                    expected: Expected::Tag("INSERT 0 400"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v=concat('changed',pk) WHERE pk%10=0;"#,
                    expected: Expected::Tag("UPDATE 40"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk%10=1;"#,
                    expected: Expected::Tag("DELETE 40"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (401,'value401'),(402,'value402'),(403,'value403'),(404,'value404'),(405,'value405'),(406,'value406'),(407,'value407'),(408,'value408'),(409,'value409'),(410,'value410'),(411,'value411'),(412,'value412'),(413,'value413'),(414,'value414'),(415,'value415'),(416,'value416'),(417,'value417'),(418,'value418'),(419,'value419'),(420,'value420'),(421,'value421'),(422,'value422'),(423,'value423'),(424,'value424'),(425,'value425'),(426,'value426'),(427,'value427'),(428,'value428'),(429,'value429'),(430,'value430'),(431,'value431'),(432,'value432'),(433,'value433'),(434,'value434'),(435,'value435'),(436,'value436'),(437,'value437'),(438,'value438'),(439,'value439'),(440,'value440'),(441,'value441'),(442,'value442'),(443,'value443'),(444,'value444'),(445,'value445'),(446,'value446'),(447,'value447'),(448,'value448'),(449,'value449'),(450,'value450');"#,
                    expected: Expected::Tag("INSERT 0 50"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','target');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('P|',statement_order,'|',table_name,'|',diff_type,'|',lower(upper(encode((replace(replace(statement,chr(96),''),' ',''))::text::bytea, 'hex')))) FROM dolt_patch('HEAD~1','HEAD') WHERE diff_type='data';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"P|1|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d313b"#)],
                            &[T(r#"P|2|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643130275748455245706b3d31303b"#)],
                            &[T(r#"P|3|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d31313b"#)],
                            &[T(r#"P|4|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643230275748455245706b3d32303b"#)],
                            &[T(r#"P|5|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d32313b"#)],
                            &[T(r#"P|6|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643330275748455245706b3d33303b"#)],
                            &[T(r#"P|7|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d33313b"#)],
                            &[T(r#"P|8|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643430275748455245706b3d34303b"#)],
                            &[T(r#"P|9|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d34313b"#)],
                            &[T(r#"P|10|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643530275748455245706b3d35303b"#)],
                            &[T(r#"P|11|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d35313b"#)],
                            &[T(r#"P|12|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643630275748455245706b3d36303b"#)],
                            &[T(r#"P|13|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d36313b"#)],
                            &[T(r#"P|14|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643730275748455245706b3d37303b"#)],
                            &[T(r#"P|15|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d37313b"#)],
                            &[T(r#"P|16|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643830275748455245706b3d38303b"#)],
                            &[T(r#"P|17|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d38313b"#)],
                            &[T(r#"P|18|public.t|data|5550444154457075626c69632e74534554763d276368616e6765643930275748455245706b3d39303b"#)],
                            &[T(r#"P|19|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d39313b"#)],
                            &[T(r#"P|20|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313030275748455245706b3d3130303b"#)],
                            &[T(r#"P|21|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3130313b"#)],
                            &[T(r#"P|22|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313130275748455245706b3d3131303b"#)],
                            &[T(r#"P|23|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3131313b"#)],
                            &[T(r#"P|24|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313230275748455245706b3d3132303b"#)],
                            &[T(r#"P|25|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3132313b"#)],
                            &[T(r#"P|26|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313330275748455245706b3d3133303b"#)],
                            &[T(r#"P|27|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3133313b"#)],
                            &[T(r#"P|28|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313430275748455245706b3d3134303b"#)],
                            &[T(r#"P|29|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3134313b"#)],
                            &[T(r#"P|30|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313530275748455245706b3d3135303b"#)],
                            &[T(r#"P|31|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3135313b"#)],
                            &[T(r#"P|32|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313630275748455245706b3d3136303b"#)],
                            &[T(r#"P|33|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3136313b"#)],
                            &[T(r#"P|34|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313730275748455245706b3d3137303b"#)],
                            &[T(r#"P|35|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3137313b"#)],
                            &[T(r#"P|36|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313830275748455245706b3d3138303b"#)],
                            &[T(r#"P|37|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3138313b"#)],
                            &[T(r#"P|38|public.t|data|5550444154457075626c69632e74534554763d276368616e676564313930275748455245706b3d3139303b"#)],
                            &[T(r#"P|39|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3139313b"#)],
                            &[T(r#"P|40|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323030275748455245706b3d3230303b"#)],
                            &[T(r#"P|41|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3230313b"#)],
                            &[T(r#"P|42|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323130275748455245706b3d3231303b"#)],
                            &[T(r#"P|43|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3231313b"#)],
                            &[T(r#"P|44|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323230275748455245706b3d3232303b"#)],
                            &[T(r#"P|45|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3232313b"#)],
                            &[T(r#"P|46|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323330275748455245706b3d3233303b"#)],
                            &[T(r#"P|47|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3233313b"#)],
                            &[T(r#"P|48|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323430275748455245706b3d3234303b"#)],
                            &[T(r#"P|49|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3234313b"#)],
                            &[T(r#"P|50|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323530275748455245706b3d3235303b"#)],
                            &[T(r#"P|51|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3235313b"#)],
                            &[T(r#"P|52|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323630275748455245706b3d3236303b"#)],
                            &[T(r#"P|53|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3236313b"#)],
                            &[T(r#"P|54|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323730275748455245706b3d3237303b"#)],
                            &[T(r#"P|55|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3237313b"#)],
                            &[T(r#"P|56|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323830275748455245706b3d3238303b"#)],
                            &[T(r#"P|57|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3238313b"#)],
                            &[T(r#"P|58|public.t|data|5550444154457075626c69632e74534554763d276368616e676564323930275748455245706b3d3239303b"#)],
                            &[T(r#"P|59|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3239313b"#)],
                            &[T(r#"P|60|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333030275748455245706b3d3330303b"#)],
                            &[T(r#"P|61|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3330313b"#)],
                            &[T(r#"P|62|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333130275748455245706b3d3331303b"#)],
                            &[T(r#"P|63|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3331313b"#)],
                            &[T(r#"P|64|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333230275748455245706b3d3332303b"#)],
                            &[T(r#"P|65|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3332313b"#)],
                            &[T(r#"P|66|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333330275748455245706b3d3333303b"#)],
                            &[T(r#"P|67|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3333313b"#)],
                            &[T(r#"P|68|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333430275748455245706b3d3334303b"#)],
                            &[T(r#"P|69|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3334313b"#)],
                            &[T(r#"P|70|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333530275748455245706b3d3335303b"#)],
                            &[T(r#"P|71|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3335313b"#)],
                            &[T(r#"P|72|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333630275748455245706b3d3336303b"#)],
                            &[T(r#"P|73|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3336313b"#)],
                            &[T(r#"P|74|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333730275748455245706b3d3337303b"#)],
                            &[T(r#"P|75|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3337313b"#)],
                            &[T(r#"P|76|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333830275748455245706b3d3338303b"#)],
                            &[T(r#"P|77|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3338313b"#)],
                            &[T(r#"P|78|public.t|data|5550444154457075626c69632e74534554763d276368616e676564333930275748455245706b3d3339303b"#)],
                            &[T(r#"P|79|public.t|data|44454c45544546524f4d7075626c69632e745748455245706b3d3339313b"#)],
                            &[T(r#"P|80|public.t|data|5550444154457075626c69632e74534554763d276368616e676564343030275748455245706b3d3430303b"#)],
                            &[T(r#"P|81|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430312c2776616c756534303127293b"#)],
                            &[T(r#"P|82|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430322c2776616c756534303227293b"#)],
                            &[T(r#"P|83|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430332c2776616c756534303327293b"#)],
                            &[T(r#"P|84|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430342c2776616c756534303427293b"#)],
                            &[T(r#"P|85|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430352c2776616c756534303527293b"#)],
                            &[T(r#"P|86|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430362c2776616c756534303627293b"#)],
                            &[T(r#"P|87|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430372c2776616c756534303727293b"#)],
                            &[T(r#"P|88|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430382c2776616c756534303827293b"#)],
                            &[T(r#"P|89|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283430392c2776616c756534303927293b"#)],
                            &[T(r#"P|90|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431302c2776616c756534313027293b"#)],
                            &[T(r#"P|91|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431312c2776616c756534313127293b"#)],
                            &[T(r#"P|92|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431322c2776616c756534313227293b"#)],
                            &[T(r#"P|93|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431332c2776616c756534313327293b"#)],
                            &[T(r#"P|94|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431342c2776616c756534313427293b"#)],
                            &[T(r#"P|95|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431352c2776616c756534313527293b"#)],
                            &[T(r#"P|96|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431362c2776616c756534313627293b"#)],
                            &[T(r#"P|97|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431372c2776616c756534313727293b"#)],
                            &[T(r#"P|98|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431382c2776616c756534313827293b"#)],
                            &[T(r#"P|99|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283431392c2776616c756534313927293b"#)],
                            &[T(r#"P|100|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432302c2776616c756534323027293b"#)],
                            &[T(r#"P|101|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432312c2776616c756534323127293b"#)],
                            &[T(r#"P|102|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432322c2776616c756534323227293b"#)],
                            &[T(r#"P|103|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432332c2776616c756534323327293b"#)],
                            &[T(r#"P|104|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432342c2776616c756534323427293b"#)],
                            &[T(r#"P|105|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432352c2776616c756534323527293b"#)],
                            &[T(r#"P|106|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432362c2776616c756534323627293b"#)],
                            &[T(r#"P|107|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432372c2776616c756534323727293b"#)],
                            &[T(r#"P|108|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432382c2776616c756534323827293b"#)],
                            &[T(r#"P|109|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283432392c2776616c756534323927293b"#)],
                            &[T(r#"P|110|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433302c2776616c756534333027293b"#)],
                            &[T(r#"P|111|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433312c2776616c756534333127293b"#)],
                            &[T(r#"P|112|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433322c2776616c756534333227293b"#)],
                            &[T(r#"P|113|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433332c2776616c756534333327293b"#)],
                            &[T(r#"P|114|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433342c2776616c756534333427293b"#)],
                            &[T(r#"P|115|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433352c2776616c756534333527293b"#)],
                            &[T(r#"P|116|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433362c2776616c756534333627293b"#)],
                            &[T(r#"P|117|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433372c2776616c756534333727293b"#)],
                            &[T(r#"P|118|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433382c2776616c756534333827293b"#)],
                            &[T(r#"P|119|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283433392c2776616c756534333927293b"#)],
                            &[T(r#"P|120|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434302c2776616c756534343027293b"#)],
                            &[T(r#"P|121|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434312c2776616c756534343127293b"#)],
                            &[T(r#"P|122|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434322c2776616c756534343227293b"#)],
                            &[T(r#"P|123|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434332c2776616c756534343327293b"#)],
                            &[T(r#"P|124|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434342c2776616c756534343427293b"#)],
                            &[T(r#"P|125|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434352c2776616c756534343527293b"#)],
                            &[T(r#"P|126|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434362c2776616c756534343627293b"#)],
                            &[T(r#"P|127|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434372c2776616c756534343727293b"#)],
                            &[T(r#"P|128|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434382c2776616c756534343827293b"#)],
                            &[T(r#"P|129|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283434392c2776616c756534343927293b"#)],
                            &[T(r#"P|130|public.t|data|494e53455254494e544f7075626c69632e7428706b2c762956414c554553283435302c2776616c756534353027293b"#)],
                        ],
                        tag: "SELECT 130",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Doltgrust writes patch statements in Postgres' SQL, where Dolt writes MySQL's"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"no_arguments"#,
            intent: r#"Doltlite oracle case: no arguments."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch();"#,
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"function dolt_patch() does not exist"#, hint: r#"No function matches the given name and argument types. You might need to add explicit type casts."#, position: 15, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"one_non_range_argument"#,
            intent: r#"Doltlite oracle case: one non range argument."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('t');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"function 'dolt_patch' expected 2 or 3 arguments, 1 received"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"bad_from_ref"#,
            intent: r#"Doltlite oracle case: bad from ref."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('missing-ref','HEAD','t');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found: missing-ref"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"bad_to_ref"#,
            intent: r#"Doltlite oracle case: bad to ref."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD','missing-ref','t');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found: missing-ref"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"missing_table"#,
            intent: r#"Doltlite oracle case: missing table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD~1','HEAD','missing_table');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "missing_table" does not exist"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"empty_range_right"#,
            intent: r#"Doltlite oracle case: empty range right."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD~1..');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"expected strings for from and to revisions, got: HEAD~1, "#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"empty_range_left"#,
            intent: r#"Doltlite oracle case: empty range left."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('..HEAD');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"expected strings for from and to revisions, got: , HEAD"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"empty_three_dot_range_right"#,
            intent: r#"Doltlite oracle case: empty three dot range right."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD~1...');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found: "#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"empty_three_dot_range_left"#,
            intent: r#"Doltlite oracle case: empty three dot range left."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('...HEAD');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found: "#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"four_arguments"#,
            intent: r#"Doltlite oracle case: four arguments."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD~1','HEAD','t','extra');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"function 'dolt_patch' expected 2 or 3 arguments, 4 received"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"null_arguments"#,
            intent: r#"Doltlite oracle case: null arguments."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch(NULL,NULL,NULL);"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"Invalid argument to dolt_patch: NULL"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"numeric_from_ref"#,
            intent: r#"Doltlite oracle case: numeric from ref."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch(123,'HEAD','t');"#,
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"function dolt_patch(integer, unknown, unknown) does not exist"#, hint: r#"No function matches the given name and argument types. You might need to add explicit type casts."#, position: 15, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"numeric_to_ref"#,
            intent: r#"Doltlite oracle case: numeric to ref."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD~1',123,'t');"#,
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"function dolt_patch(unknown, integer, unknown) does not exist"#, hint: r#"No function matches the given name and argument types. You might need to add explicit type casts."#, position: 15, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"numeric_table"#,
            intent: r#"Doltlite oracle case: numeric table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD~1','HEAD',123);"#,
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"function dolt_patch(unknown, unknown, integer) does not exist"#, hint: r#"No function matches the given name and argument types. You might need to add explicit type casts."#, position: 15, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"bad_range_from_ref"#,
            intent: r#"Doltlite oracle case: bad range from ref."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('missing-ref..HEAD','t');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found: missing-ref"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"bad_range_to_ref"#,
            intent: r#"Doltlite oracle case: bad range to ref."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(pk integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','base');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','next');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD~1..missing-ref','t');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found: missing-ref"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
    ]);
}
