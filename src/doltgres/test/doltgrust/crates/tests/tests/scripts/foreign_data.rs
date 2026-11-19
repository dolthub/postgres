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
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};


#[test]
fn test_foreign_data_wrappers_and_servers() {
    run_scripts(&[
        ScriptTest {
            name: "foreign-data wrappers and servers: options, owners, privileges, renames, and drops",
            set_up_script: &[
                "CREATE ROLE regress_fdw_user LOGIN SUPERUSER;",
                "SET SESSION AUTHORIZATION 'regress_fdw_user';",
                "CREATE ROLE regress_fdw_role;",
                "CREATE ROLE regress_fdw_super SUPERUSER;",
                "CREATE FUNCTION invalid_fdw_handler() RETURNS int LANGUAGE SQL AS 'SELECT 1;';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER dummy;",
                    expected: Expected::Tag("CREATE FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON FOREIGN DATA WRAPPER dummy IS 'useless';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER postgresql VALIDATOR postgresql_fdw_validator;",
                    expected: Expected::Tag("CREATE FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fdwname, fdwhandler::regproc, fdwvalidator::regproc, fdwoptions FROM pg_foreign_data_wrapper ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fdwname", NAME), Column("fdwhandler", REGPROC), Column("fdwvalidator", REGPROC), Column("fdwoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("dummy"), T("-"), T("-"), Null],
                            &[T("postgresql"), T("-"), T("postgresql_fdw_validator"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER foo VALIDATOR bar;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function bar(text[], oid) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Tag("CREATE FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "foreign-data wrapper \"foo\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Tag("DROP FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER foo OPTIONS (testing '1', testing '2');",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "option \"testing\" provided more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER foo OPTIONS (testing '1', another '2');",
                    expected: Expected::Tag("CREATE FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fdwname, fdwoptions FROM pg_foreign_data_wrapper WHERE fdwname = 'foo';",
                    expected: Expected::Rows {
                        columns: &[Column("fdwname", NAME), Column("fdwoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("foo"), T("{testing=1,another=2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Tag("DROP FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_fdw_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create foreign-data wrapper \"foo\"", hint: "Must be superuser to create a foreign-data wrapper.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER foo VALIDATOR postgresql_fdw_validator;",
                    expected: Expected::Tag("CREATE FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER test_fdw HANDLER invalid_fdw_handler;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "function invalid_fdw_handler must return type fdw_handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN DATA WRAPPER test_fdw HANDLER invalid_fdw_handler HANDLER invalid_fdw_handler;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "function invalid_fdw_handler must return type fdw_handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (nonexistent 'fdw');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"nonexistent\"", hint: "There are no valid options in this context.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo VALIDATOR bar;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function bar(text[], oid) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo NO VALIDATOR;",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (a '1', b '2');",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (SET c '4');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "option \"c\" not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (DROP c);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "option \"c\" not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD x '1', DROP x);",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (DROP a, SET b '3', ADD c '4');",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (a '2');",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (b '4');",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "option \"b\" provided more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fdwname, fdwvalidator::regproc, fdwoptions FROM pg_foreign_data_wrapper WHERE fdwname = 'foo';",
                    expected: Expected::Rows {
                        columns: &[Column("fdwname", NAME), Column("fdwvalidator", REGPROC), Column("fdwoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("foo"), T("-"), T("{b=3,c=4,a=2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_fdw_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD d '5');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter foreign-data wrapper \"foo\"", hint: "Must be superuser to alter a foreign-data wrapper.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_fdw_super;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD d '5');",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OWNER TO regress_fdw_role;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to change owner of foreign-data wrapper \"foo\"", hint: "The owner of a foreign-data wrapper must be a superuser.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OWNER TO regress_fdw_super;",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fdwname, fdwowner::regrole, fdwoptions FROM pg_foreign_data_wrapper WHERE fdwname = 'foo';",
                    expected: Expected::Rows {
                        columns: &[Column("fdwname", NAME), Column("fdwowner", REGROLE), Column("fdwoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("foo"), T("regress_fdw_super"), T("{b=3,c=4,a=2,d=5}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo RENAME TO foo1;",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo1 RENAME TO dummy;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "foreign-data wrapper \"dummy\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo1 RENAME TO foo;",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo HANDLER invalid_fdw_handler;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "function invalid_fdw_handler must return type fdw_handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN DATA WRAPPER nonexistent;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "foreign-data wrapper \"nonexistent\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN DATA WRAPPER IF EXISTS nonexistent;",
                    notices: &[Diagnostic { code: "00000", message: "foreign-data wrapper \"nonexistent\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN DATA WRAPPER foo OWNER TO regress_fdw_user;",
                    expected: Expected::Tag("ALTER FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s1 FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON SERVER s1 IS 'foreign server';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s1 FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "server \"s1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER IF NOT EXISTS s1 FOREIGN DATA WRAPPER foo;",
                    notices: &[Diagnostic { code: "42710", message: "server \"s1\" already exists, skipping", ..N }],
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s2 FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s3 TYPE 'oracle' FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s4 TYPE 'oracle' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s5 VERSION '15.0' FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s6 VERSION '16.0' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s7 TYPE 'oracle' VERSION '17.0' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s8 FOREIGN DATA WRAPPER postgresql OPTIONS (foo '1');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"foo\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s8 FOREIGN DATA WRAPPER postgresql OPTIONS (host 'localhost', dbname 's8db');",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s9 FOREIGN DATA WRAPPER postgresql OPTIONS (hostt 'localhost');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"hostt\"", hint: "Perhaps you meant the option \"host\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_fdw_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER t1 FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for foreign-data wrapper foo", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_fdw_role;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_fdw_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER t1 FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON FOREIGN DATA WRAPPER foo FROM regress_fdw_role;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_fdw_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER t2 FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for foreign-data wrapper foo", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, srvowner::regrole, srvtype, srvversion, srvoptions, obj_description(oid, 'pg_foreign_server') FROM pg_foreign_server ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("srvowner", REGROLE), Column("srvtype", TEXT), Column("srvversion", TEXT), Column("srvoptions", TEXT_ARRAY), Column("obj_description", TEXT)],
                        rows: &[
                            &[T("s1"), T("regress_fdw_user"), Null, Null, Null, T("foreign server")],
                            &[T("s2"), T("regress_fdw_user"), Null, Null, T("{host=a,dbname=b}"), Null],
                            &[T("s3"), T("regress_fdw_user"), T("oracle"), Null, Null, Null],
                            &[T("s4"), T("regress_fdw_user"), T("oracle"), Null, T("{host=a,dbname=b}"), Null],
                            &[T("s5"), T("regress_fdw_user"), Null, T("15.0"), Null, Null],
                            &[T("s6"), T("regress_fdw_user"), Null, T("16.0"), T("{host=a,dbname=b}"), Null],
                            &[T("s7"), T("regress_fdw_user"), T("oracle"), T("17.0"), T("{host=a,dbname=b}"), Null],
                            &[T("s8"), T("regress_fdw_user"), Null, Null, T("{host=localhost,dbname=s8db}"), Null],
                            &[T("t1"), T("regress_fdw_role"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s0;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \";\"", position: 16, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s0 OPTIONS (a '1');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "server \"s0\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s1 VERSION '1.0' OPTIONS (servername 's1');",
                    expected: Expected::Tag("ALTER SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s2 VERSION '1.1';",
                    expected: Expected::Tag("ALTER SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s3 OPTIONS (\"tns name\" 'orcl', port '1521');",
                    expected: Expected::Tag("ALTER SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON FOREIGN SERVER s1 TO regress_fdw_role;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s8 OPTIONS (foo '1');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"foo\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s8 OPTIONS (connect_timeout '30', SET dbname 'db1', DROP host);",
                    expected: Expected::Tag("ALTER SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_fdw_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s1 VERSION '1.1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of foreign server s1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s1 OWNER TO regress_fdw_role;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of foreign server s1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s1 OWNER TO regress_fdw_role;",
                    expected: Expected::Tag("ALTER SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, srvowner::regrole, srvversion, srvacl, srvoptions FROM pg_foreign_server WHERE srvname IN ('s1', 's8', 's3') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("srvowner", REGROLE), Column("srvversion", TEXT), Column("srvacl", ACLITEM_ARRAY), Column("srvoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("s1"), T("regress_fdw_role"), T("1.0"), T("{regress_fdw_role=U/regress_fdw_role}"), T("{servername=s1}")],
                            &[T("s3"), T("regress_fdw_user"), Null, Null, T("{\"tns name=orcl\",port=1521}")],
                            &[T("s8"), T("regress_fdw_user"), Null, Null, T("{dbname=db1,connect_timeout=30}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s8 RENAME to s8new;",
                    expected: Expected::Tag("ALTER SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SERVER s8new RENAME to s1;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "server \"s1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fdwname, fdwacl FROM pg_foreign_data_wrapper ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("fdwname", NAME), Column("fdwacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dummy"), Null],
                            &[T("foo"), T("{regress_fdw_user=U/regress_fdw_user}")],
                            &[T("postgresql"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_foreign_data_wrapper_privilege('regress_fdw_role', 'foo', 'USAGE'), has_server_privilege('regress_fdw_role', 's1', 'USAGE'), has_server_privilege('regress_fdw_role', 's2', 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_foreign_data_wrapper_privilege", BOOL), Column("has_server_privilege", BOOL), Column("has_server_privilege", BOOL)],
                        rows: &[
                            &[T("f"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreign_data_wrapper_name, authorization_identifier, library_name, foreign_data_wrapper_language FROM information_schema.foreign_data_wrappers ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("foreign_data_wrapper_name", NAME), Column("authorization_identifier", NAME), Column("library_name", VARCHAR), Column("foreign_data_wrapper_language", VARCHAR)],
                        rows: &[
                            &[T("dummy"), T("regress_fdw_user"), Null, T("c")],
                            &[T("foo"), T("regress_fdw_user"), Null, T("c")],
                            &[T("postgresql"), T("regress_fdw_user"), Null, T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreign_server_name, foreign_data_wrapper_name, foreign_server_type, foreign_server_version, authorization_identifier FROM information_schema.foreign_servers ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("foreign_server_name", NAME), Column("foreign_data_wrapper_name", NAME), Column("foreign_server_type", VARCHAR), Column("foreign_server_version", VARCHAR), Column("authorization_identifier", NAME)],
                        rows: &[
                            &[T("s1"), T("foo"), Null, T("1.0"), T("regress_fdw_role")],
                            &[T("s2"), T("foo"), Null, T("1.1"), T("regress_fdw_user")],
                            &[T("s3"), T("foo"), T("oracle"), Null, T("regress_fdw_user")],
                            &[T("s4"), T("foo"), T("oracle"), Null, T("regress_fdw_user")],
                            &[T("s5"), T("foo"), Null, T("15.0"), T("regress_fdw_user")],
                            &[T("s6"), T("foo"), Null, T("16.0"), T("regress_fdw_user")],
                            &[T("s7"), T("foo"), T("oracle"), T("17.0"), T("regress_fdw_user")],
                            &[T("s8new"), T("postgresql"), Null, Null, T("regress_fdw_user")],
                            &[T("t1"), T("foo"), Null, Null, T("regress_fdw_role")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreign_server_name, option_name, option_value FROM information_schema.foreign_server_options ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("foreign_server_name", NAME), Column("option_name", NAME), Column("option_value", VARCHAR)],
                        rows: &[
                            &[T("s1"), T("servername"), T("s1")],
                            &[T("s2"), T("dbname"), T("b")],
                            &[T("s2"), T("host"), T("a")],
                            &[T("s3"), T("port"), T("1521")],
                            &[T("s3"), T("tns name"), T("orcl")],
                            &[T("s4"), T("dbname"), T("b")],
                            &[T("s4"), T("host"), T("a")],
                            &[T("s6"), T("dbname"), T("b")],
                            &[T("s6"), T("host"), T("a")],
                            &[T("s7"), T("dbname"), T("b")],
                            &[T("s7"), T("host"), T("a")],
                            &[T("s8new"), T("connect_timeout"), T("30")],
                            &[T("s8new"), T("dbname"), T("db1")],
                        ],
                        tag: "SELECT 13",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT option_name, option_value FROM pg_options_to_table((SELECT srvoptions FROM pg_foreign_server WHERE srvname = 's8new')) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("option_name", TEXT), Column("option_value", TEXT)],
                        rows: &[
                            &[T("connect_timeout"), T("30")],
                            &[T("dbname"), T("db1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid), pg_identify_object_as_address(classid, objid, objsubid) FROM pg_get_object_address('server', '{s3}', '{}');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_identify_object_as_address", RECORD)],
                        rows: &[
                            &[T("server s3"), T("(server,{s3},{})")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SERVER nonexistent;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "server \"nonexistent\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SERVER IF EXISTS nonexistent;",
                    notices: &[Diagnostic { code: "00000", message: "server \"nonexistent\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_fdw_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SERVER s2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of foreign server s2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN DATA WRAPPER foo;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop foreign-data wrapper foo because other objects depend on it", detail: "server s1 depends on foreign-data wrapper foo\nserver s2 depends on foreign-data wrapper foo\nserver s3 depends on foreign-data wrapper foo\nserver s4 depends on foreign-data wrapper foo\nserver s5 depends on foreign-data wrapper foo\nserver s6 depends on foreign-data wrapper foo\nserver s7 depends on foreign-data wrapper foo\nserver t1 depends on foreign-data wrapper foo", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN DATA WRAPPER foo CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 8 other objects", detail: "drop cascades to server s1\ndrop cascades to server s2\ndrop cascades to server s3\ndrop cascades to server s4\ndrop cascades to server s5\ndrop cascades to server s6\ndrop cascades to server s7\ndrop cascades to server t1", ..N }],
                    expected: Expected::Tag("DROP FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_foreign_server;",
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
                    query: "DROP FOREIGN DATA WRAPPER dummy;",
                    expected: Expected::Tag("DROP FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN DATA WRAPPER postgresql;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop foreign-data wrapper postgresql because other objects depend on it", detail: "server s8new depends on foreign-data wrapper postgresql", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN DATA WRAPPER postgresql CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to server s8new", ..N }],
                    expected: Expected::Tag("DROP FOREIGN DATA WRAPPER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION invalid_fdw_handler();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_fdw_role;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_fdw_super;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_fdw_user;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_foreign_tables() {
    run_scripts(&[
        ScriptTest {
            name: "foreign tables: options, column options, constraints, privileges, handlers, and drops",
            set_up_script: &[
                "CREATE ROLE regress_ft_user LOGIN SUPERUSER;",
                "SET SESSION AUTHORIZATION 'regress_ft_user';",
                "CREATE ROLE regress_ft_role;",
                "CREATE FOREIGN DATA WRAPPER dummy;",
                "CREATE FOREIGN DATA WRAPPER postgresql VALIDATOR postgresql_fdw_validator;",
                "CREATE SERVER s0 FOREIGN DATA WRAPPER dummy;",
                "CREATE SERVER sp FOREIGN DATA WRAPPER postgresql;",
                "CREATE TABLE lt (a int GENERATED ALWAYS AS IDENTITY, b text DEFAULT 'x' NOT NULL, CHECK (a > 0));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft1 (c1 integer OPTIONS (\"param 1\" 'val1') NOT NULL, c2 text OPTIONS (param2 'val2', param3 'val3') CHECK (c2 <> ''), c3 date, CHECK (c3 BETWEEN '1994-01-01'::date AND '1994-01-31'::date)) SERVER s0 OPTIONS (delimiter ',', quote '\"', \"be quoted\" 'value');",
                    expected: Expected::Tag("CREATE FOREIGN TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relkind, s.srvname, t.ftoptions FROM pg_foreign_table t JOIN pg_class c ON c.oid = t.ftrelid JOIN pg_foreign_server s ON s.oid = t.ftserver;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relkind", CHAR), Column("srvname", NAME), Column("ftoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("ft1"), T("f"), T("s0"), T("{\"delimiter=,\",\"quote=\\\"\",\"be quoted=value\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attnotnull, attfdwoptions FROM pg_attribute WHERE attrelid = 'ft1'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("attfdwoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("c1"), T("t"), T("{\"param 1=val1\"}")],
                            &[T("c2"), T("f"), T("{param2=val2,param3=val3}")],
                            &[T("c3"), T("f"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, contype, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'ft1'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("contype", CHAR), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("ft1_c1_not_null"), T("n"), T("NOT NULL c1")],
                            &[T("ft1_c2_check"), T("c"), T("CHECK ((c2 <> ''::text))")],
                            &[T("ft1_c3_check"), T("c"), T("CHECK (((c3 >= '1994-01-01'::date) AND (c3 <= '1994-01-31'::date)))")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft2 (c1 integer) SERVER no_server;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "server \"no_server\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft2 (c1 integer PRIMARY KEY) SERVER s0;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "primary key constraints are not supported on foreign tables", position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft2 (c1 integer, UNIQUE (c1)) SERVER s0;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unique constraints are not supported on foreign tables", position: 39, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft2 (c1 integer REFERENCES lt) SERVER s0;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "foreign key constraints are not supported on foreign tables", position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft2 (c1 integer) SERVER sp OPTIONS (foo 'bar');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"foo\"", hint: "There are no valid options in this context.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft2 (c1 integer OPTIONS (foo 'bar')) SERVER sp;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"foo\"", hint: "There are no valid options in this context.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft2 (c1 integer) SERVER s0 OPTIONS (a '1', a '2');",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "option \"a\" provided more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft2 (LIKE lt INCLUDING ALL) SERVER s0;",
                    expected: Expected::Tag("CREATE FOREIGN TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attidentity, attnotnull, pg_get_expr(d.adbin, d.adrelid) FROM pg_attribute a LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum WHERE attrelid = 'ft2'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attidentity", CHAR), Column("attnotnull", BOOL), Column("pg_get_expr", TEXT)],
                        rows: &[
                            &[T("a"), T(""), T("t"), Null],
                            &[T("b"), T(""), T("t"), T("'x'::text")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft1 OPTIONS (DROP delimiter, SET quote '~', ADD escape '@');",
                    expected: Expected::Tag("ALTER FOREIGN TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ftoptions FROM pg_foreign_table WHERE ftrelid = 'ft1'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("ftoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("{quote=~,\"be quoted=value\",escape=@}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft1 OPTIONS (ADD quote '!');",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "option \"quote\" provided more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft1 OPTIONS (DROP nothing);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "option \"nothing\" not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft1 OPTIONS (SET nothing '1');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "option \"nothing\" not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft1 ALTER COLUMN c1 OPTIONS (SET \"param 1\" 'changed', ADD p4 'v4'), ALTER COLUMN c2 OPTIONS (DROP param2), ALTER COLUMN c3 OPTIONS (ADD p5 'v5');",
                    expected: Expected::Tag("ALTER FOREIGN TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attfdwoptions FROM pg_attribute WHERE attrelid = 'ft1'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attfdwoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("c1"), T("{\"param 1=changed\",p4=v4}")],
                            &[T("c2"), T("{param3=val3}")],
                            &[T("c3"), T("{p5=v5}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft1 ALTER COLUMN no_column OPTIONS (ADD p 'v');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"no_column\" of relation \"ft1\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft1 ALTER COLUMN c1 OPTIONS (DROP p6);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "option \"p6\" not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE lt OPTIONS (ADD a '1');",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "ALTER action OPTIONS cannot be performed on relation \"lt\"", detail: "This operation is not supported for tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE lt ALTER COLUMN a OPTIONS (ADD a '1');",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "ALTER action ALTER COLUMN ... OPTIONS cannot be performed on relation \"lt\"", detail: "This operation is not supported for tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft3 (c1 integer) SERVER sp;",
                    expected: Expected::Tag("CREATE FOREIGN TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft3 OPTIONS (ADD foo 'bar');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"foo\"", hint: "There are no valid options in this context.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft3 ALTER COLUMN c1 OPTIONS (ADD foo 'bar');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"foo\"", hint: "There are no valid options in this context.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ft1;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "foreign-data wrapper \"dummy\" has no handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM ft1;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "foreign-data wrapper \"dummy\" has no handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ft1 VALUES (1, 'a', '1994-01-02');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "foreign-data wrapper \"dummy\" has no handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ft1 SET c1 = 2;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "foreign-data wrapper \"dummy\" has no handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ft1;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "foreign-data wrapper \"dummy\" has no handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET restrict_nonsystem_relation_kind = 'foreign-table';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ft1;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "access to non-system foreign table is restricted", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET restrict_nonsystem_relation_kind;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "IMPORT FOREIGN SCHEMA s1 FROM SERVER s0 INTO public;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "foreign-data wrapper \"dummy\" has no handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "IMPORT FOREIGN SCHEMA s1 LIMIT TO (t1) FROM SERVER no_server INTO public;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "server \"no_server\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "IMPORT FOREIGN SCHEMA s1 FROM SERVER s0 INTO no_schema;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: "schema \"no_schema\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON FOREIGN SERVER s0 TO regress_ft_role;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO regress_ft_role;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_ft_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft4 (c1 integer) SERVER sp;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for foreign server sp", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FOREIGN TABLE ft4 (c1 integer) SERVER s0;",
                    expected: Expected::Tag("CREATE FOREIGN TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "IMPORT FOREIGN SCHEMA s1 FROM SERVER sp INTO public;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for foreign server sp", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FOREIGN TABLE ft1 OPTIONS (ADD x 'y');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of foreign table ft1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_ft_role;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, srvacl FROM pg_foreign_server ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("srvacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("s0"), T("{regress_ft_user=U/regress_ft_user}")],
                            &[T("sp"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SERVER s0;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop server s0 because other objects depend on it", detail: "foreign table ft1 depends on server s0\nforeign table ft2 depends on server s0", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SERVER s0 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 2 other objects", detail: "drop cascades to foreign table ft1\ndrop cascades to foreign table ft2", ..N }],
                    expected: Expected::Tag("DROP SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname FROM pg_foreign_table t JOIN pg_class c ON c.oid = t.ftrelid ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("ft3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FOREIGN TABLE ft3;",
                    expected: Expected::Tag("DROP FOREIGN TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_foreign_table;",
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

#[test]
fn test_user_mappings() {
    run_scripts(&[
        ScriptTest {
            name: "user mappings: options, their hiding from other roles, privileges, dependencies, and drops",
            set_up_script: &[
                "CREATE ROLE regress_um_user LOGIN SUPERUSER;",
                "SET SESSION AUTHORIZATION 'regress_um_user';",
                "CREATE ROLE regress_um_role LOGIN;",
                "CREATE ROLE regress_um_other;",
                "CREATE FOREIGN DATA WRAPPER postgresql VALIDATOR postgresql_fdw_validator;",
                "CREATE SERVER s1 FOREIGN DATA WRAPPER postgresql;",
                "CREATE SERVER s2 FOREIGN DATA WRAPPER postgresql;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR regress_um_role SERVER s1 OPTIONS (user 'bob', password 'secret');",
                    expected: Expected::Tag("CREATE USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR PUBLIC SERVER s1 OPTIONS (user 'everyone');",
                    expected: Expected::Tag("CREATE USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR CURRENT_USER SERVER s2;",
                    expected: Expected::Tag("CREATE USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR regress_um_role SERVER s1;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "user mapping for \"regress_um_role\" already exists for server \"s1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING IF NOT EXISTS FOR regress_um_role SERVER s1;",
                    notices: &[Diagnostic { code: "42710", message: "user mapping for \"regress_um_role\" already exists for server \"s1\", skipping", ..N }],
                    expected: Expected::Tag("CREATE USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR PUBLIC SERVER s1;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "user mapping for \"public\" already exists for server \"s1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR no_role SERVER s1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "role \"no_role\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR regress_um_other SERVER no_server;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "server \"no_server\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR regress_um_other SERVER s1 OPTIONS (username 'x');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid option \"username\"", hint: "Perhaps you meant the option \"user\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR regress_um_other SERVER s1 OPTIONS (user 'a', user 'b');",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "option \"user\" provided more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, usename, umoptions FROM pg_user_mappings ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("usename", NAME), Column("umoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("s1"), T("public"), T("{user=everyone}")],
                            &[T("s1"), T("regress_um_role"), T("{user=bob,password=secret}")],
                            &[T("s2"), T("regress_um_user"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.srvname, m.umuser::regrole, m.umoptions FROM pg_user_mapping m JOIN pg_foreign_server s ON s.oid = m.umserver ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("umuser", REGROLE), Column("umoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("s1"), T("-"), T("{user=everyone}")],
                            &[T("s1"), T("regress_um_role"), T("{user=bob,password=secret}")],
                            &[T("s2"), T("regress_um_user"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR regress_um_role SERVER s1 OPTIONS (SET password 'changed', DROP user);",
                    expected: Expected::Tag("ALTER USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR PUBLIC SERVER s1 OPTIONS (ADD password 'p');",
                    expected: Expected::Tag("ALTER USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR regress_um_other SERVER s1 OPTIONS (ADD user 'x');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "user mapping for \"regress_um_other\" does not exist for server \"s1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR regress_um_role SERVER no_server OPTIONS (ADD user 'x');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "server \"no_server\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR regress_um_role SERVER s1 OPTIONS (DROP nothing);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "option \"nothing\" not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR CURRENT_USER SERVER s2 OPTIONS (ADD user 'me');",
                    expected: Expected::Tag("ALTER USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR CURRENT_USER SERVER s2 OPTIONS (DROP user);",
                    expected: Expected::Tag("ALTER USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, usename, umoptions FROM pg_user_mappings ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("usename", NAME), Column("umoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("s1"), T("public"), T("{user=everyone,password=p}")],
                            &[T("s1"), T("regress_um_role"), T("{password=changed}")],
                            &[T("s2"), T("regress_um_user"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid), pg_describe_object(refclassid, refobjid, refobjsubid), deptype FROM pg_depend WHERE classid = 'pg_user_mapping'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_describe_object", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("user mapping for public on server s1"), T("server s1"), T("n")],
                            &[T("user mapping for regress_um_role on server s1"), T("server s1"), T("n")],
                            &[T("user mapping for regress_um_user on server s2"), T("server s2"), T("n")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object(a.classid, a.objid, a.objsubid)).*, (pg_identify_object_as_address(a.classid, a.objid, a.objsubid)).* FROM pg_get_object_address('user mapping', '{regress_um_role}', '{s1}') a;",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT), Column("type", TEXT), Column("object_names", TEXT_ARRAY), Column("object_args", TEXT_ARRAY)],
                        rows: &[
                            &[T("user mapping"), Null, Null, T("regress_um_role on server s1"), T("user mapping"), T("{regress_um_role}"), T("{s1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_object_address('user mapping', '{no_role}', '{s1}');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "user mapping for user \"no_role\" on server \"s1\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_object_address('user mapping', '{regress_um_other}', '{s1}');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "user mapping for user \"regress_um_other\" on server \"s1\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_object_address('user mapping', '{regress_um_role}', '{no_server}');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "server \"no_server\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON FOREIGN SERVER s1 TO regress_um_role;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_um_role;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, usename, umoptions FROM pg_user_mappings ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("usename", NAME), Column("umoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("s1"), T("public"), Null],
                            &[T("s1"), T("regress_um_role"), T("{password=changed}")],
                            &[T("s2"), T("regress_um_user"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT authorization_identifier, foreign_server_name, option_name, option_value FROM information_schema.user_mapping_options ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("authorization_identifier", NAME), Column("foreign_server_name", NAME), Column("option_name", NAME), Column("option_value", VARCHAR)],
                        rows: &[
                            &[T("PUBLIC"), T("s1"), T("password"), Null],
                            &[T("PUBLIC"), T("s1"), T("user"), Null],
                            &[T("regress_um_role"), T("s1"), T("password"), T("changed")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_user_mapping;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pg_user_mapping", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR regress_um_role SERVER s1 OPTIONS (ADD user 'self');",
                    expected: Expected::Tag("ALTER USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER MAPPING FOR PUBLIC SERVER s1 OPTIONS (SET user 'x');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of foreign server s1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR regress_um_other SERVER s1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of foreign server s1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR regress_um_role SERVER s2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for foreign server s2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER MAPPING FOR PUBLIC SERVER s1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of foreign server s1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER MAPPING FOR regress_um_other SERVER s1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "user mapping for \"regress_um_other\" does not exist for server \"s1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER MAPPING IF EXISTS FOR regress_um_other SERVER s1;",
                    notices: &[Diagnostic { code: "00000", message: "user mapping for \"regress_um_other\" does not exist for server \"s1\", skipping", ..N }],
                    expected: Expected::Tag("DROP USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER MAPPING IF EXISTS FOR no_role SERVER s1;",
                    notices: &[Diagnostic { code: "00000", message: "role \"no_role\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER MAPPING IF EXISTS FOR regress_um_other SERVER no_server;",
                    notices: &[Diagnostic { code: "00000", message: "server \"no_server\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER MAPPING FOR regress_um_other SERVER no_server;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "server \"no_server\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SERVER s1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop server s1 because other objects depend on it", detail: "user mapping for regress_um_role on server s1 depends on server s1\nuser mapping for public on server s1 depends on server s1", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_um_role;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "role \"regress_um_role\" cannot be dropped because some objects depend on it", detail: "privileges for server s1\nowner of user mapping for regress_um_role on server s1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_um_role;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, usename FROM pg_user_mappings ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("usename", NAME)],
                        rows: &[
                            &[T("s1"), T("public")],
                            &[T("s2"), T("regress_um_user")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER MAPPING FOR CURRENT_USER SERVER s2;",
                    expected: Expected::Tag("DROP USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SERVER s1 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to user mapping for public on server s1", ..N }],
                    expected: Expected::Tag("DROP SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_user_mapping;",
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
        ScriptTest {
            name: "user mappings belong to the database: shown on the branches that have their servers",
            set_up_script: &[
                "CREATE FOREIGN DATA WRAPPER postgresql VALIDATOR postgresql_fdw_validator;",
                "CREATE SERVER s1 FOREIGN DATA WRAPPER postgresql;",
                "SELECT dolt_commit('-Am', 'wrapper and server');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SERVER s_other FOREIGN DATA WRAPPER postgresql;",
                    expected: Expected::Tag("CREATE SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR PUBLIC SERVER s_other OPTIONS (user 'x');",
                    expected: Expected::Tag("CREATE USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER MAPPING FOR PUBLIC SERVER s1;",
                    expected: Expected::Tag("CREATE USER MAPPING"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, usename FROM pg_user_mappings ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("usename", NAME)],
                        rows: &[
                            &[T("s1"), T("public")],
                            &[T("s_other"), T("public")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'servers');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, usename FROM pg_user_mappings ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("usename", NAME)],
                        rows: &[
                            &[T("s1"), T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SERVER s1 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to user mapping for public on server s1", ..N }],
                    expected: Expected::Tag("DROP SERVER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname, usename FROM pg_user_mappings ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME), Column("usename", NAME)],
                        rows: &[
                            &[T("s_other"), T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srvname FROM pg_foreign_server ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("srvname", NAME)],
                        rows: &[
                            &[T("s1")],
                            &[T("s_other")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_identify_foreign_data_wrappers_and_servers() {
    run_scripts(&[
        ScriptTest {
            name: "pg_identify_object names a foreign-data wrapper and a server",
            set_up_script: &[
                "CREATE FOREIGN DATA WRAPPER addr_fdw;",
                "CREATE SERVER addr_fserv FOREIGN DATA WRAPPER addr_fdw;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object('pg_foreign_data_wrapper'::regclass, oid, 0)).* FROM pg_foreign_data_wrapper WHERE fdwname = 'addr_fdw';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("foreign-data wrapper"), Null, T("addr_fdw"), T("addr_fdw")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object('pg_foreign_server'::regclass, oid, 0)).* FROM pg_foreign_server WHERE srvname = 'addr_fserv';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("server"), Null, T("addr_fserv"), T("addr_fserv")],
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
