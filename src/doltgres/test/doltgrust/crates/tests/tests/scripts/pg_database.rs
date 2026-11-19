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
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_pg_database() {
    run_scripts(&[
        ScriptTest {
            name: "pg_database lists the server's databases from its registry, with their owners and locales",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT oid, datname, datdba, encoding, datlocprovider, datistemplate, datallowconn, dathasloginevt, datconnlimit, datminmxid, dattablespace, datcollate, datctype, datlocale, daticurules, datcollversion FROM pg_database ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID), Column("datname", NAME), Column("datdba", OID), Column("encoding", INT4), Column("datlocprovider", CHAR), Column("datistemplate", BOOL), Column("datallowconn", BOOL), Column("dathasloginevt", BOOL), Column("datconnlimit", INT4), Column("datminmxid", XID), Column("dattablespace", OID), Column("datcollate", TEXT), Column("datctype", TEXT), Column("datlocale", TEXT), Column("daticurules", TEXT), Column("datcollversion", TEXT)],
                        rows: &[
                            &[T("1"), T("template1"), T("10"), T("6"), T("b"), T("t"), T("t"), T("f"), T("-1"), T("1"), T("1663"), T("C.UTF-8"), T("C.UTF-8"), T("C.UTF-8"), Null, T("1")],
                            &[T("4"), T("template0"), T("10"), T("6"), T("b"), T("t"), T("f"), T("f"), T("-1"), T("1"), T("1663"), T("C.UTF-8"), T("C.UTF-8"), T("C.UTF-8"), Null, Null],
                            &[T("5"), T("postgres"), T("10"), T("6"), T("b"), T("f"), T("t"), T("f"), T("-1"), T("1"), T("1663"), T("C.UTF-8"), T("C.UTF-8"), T("C.UTF-8"), Null, T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, datacl FROM pg_database ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("datacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("template1"), T("{=c/postgres,postgres=CTc/postgres}")],
                            &[T("template0"), T("{=c/postgres,postgres=CTc/postgres}")],
                            &[T("postgres"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, datfrozenxid FROM pg_database ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("datfrozenxid", XID)],
                        rows: &[
                            &[T("template1"), T("744")],
                            &[T("template0"), T("744")],
                            &[T("postgres"), T("744")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("no row of ours ever needs freezing, so datfrozenxid reports the oldest xmin running snapshots need instead of the transaction id initdb froze at"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname, pg_get_userbyid(d.datdba), pg_encoding_to_char(d.encoding) FROM pg_database d ORDER BY d.oid;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("pg_get_userbyid", NAME), Column("pg_encoding_to_char", NAME)],
                        rows: &[
                            &[T("template1"), T("postgres"), T("UTF8")],
                            &[T("template0"), T("postgres"), T("UTF8")],
                            &[T("postgres"), T("postgres"), T("UTF8")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d1;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d2 IS_TEMPLATE true CONNECTION LIMIT 5 ALLOW_CONNECTIONS false;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oid >= 16384 AS normal, datname, datdba, datistemplate, datallowconn, datconnlimit, datlocprovider FROM pg_database WHERE oid >= 16384 ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("normal", BOOL), Column("datname", NAME), Column("datdba", OID), Column("datistemplate", BOOL), Column("datallowconn", BOOL), Column("datconnlimit", INT4), Column("datlocprovider", CHAR)],
                        rows: &[
                            &[T("t"), T("d1"), T("10"), T("f"), T("t"), T("-1"), T("b")],
                            &[T("t"), T("d2"), T("10"), T("t"), T("f"), T("5"), T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE d1;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("template1")],
                            &[T("template0")],
                            &[T("postgres")],
                            &[T("d2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d3 OWNER postgres;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, pg_get_userbyid(datdba) FROM pg_database WHERE datname = 'd3';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("pg_get_userbyid", NAME)],
                        rows: &[
                            &[T("d3"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a database created from a session on one branch is in pg_database for sessions on every branch",
            set_up_script: &[
                "SELECT DOLT_CHECKOUT('-b', 'other');",
                "CREATE DATABASE onbranch;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname = 'onbranch';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("onbranch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname = 'onbranch';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("onbranch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main_branch",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
