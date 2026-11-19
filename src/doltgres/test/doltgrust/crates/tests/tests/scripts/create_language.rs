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
fn test_create_language() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE LANGUAGE with PL/pgSQL's handlers, OR REPLACE of created and built-in languages, RENAME, OWNER, and DROP",
            set_up_script: &[
                "CREATE ROLE lang_u1;",
                "CREATE ROLE lang_u2;",
                "CREATE ROLE lang_u3;",
                "GRANT lang_u3 TO lang_u1;",
                "GRANT CREATE ON SCHEMA public TO lang_u1;",
                "CREATE FUNCTION lang_sqlf(int) RETURNS int LANGUAGE sql AS 'SELECT $1 + 1';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE LANGUAGE lang_a HANDLER plpgsql_call_handler;",
                    expected: Expected::Tag("CREATE LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE LANGUAGE lang_a HANDLER plpgsql_call_handler;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "language \"lang_a\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRUSTED LANGUAGE lang_b HANDLER plpgsql_call_handler INLINE plpgsql_inline_handler VALIDATOR plpgsql_validator;",
                    expected: Expected::Tag("CREATE LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE LANGUAGE lang_c HANDLER lang_sqlf;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function lang_sqlf() does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE LANGUAGE lang_c HANDLER nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nosuch() does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE LANGUAGE lang_c HANDLER plpgsql_call_handler INLINE nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nosuch(internal) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE LANGUAGE lang_c HANDLER plpgsql_call_handler VALIDATOR plpgsql_inline_handler;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function plpgsql_inline_handler(oid) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanowner::regrole, lanispl, lanpltrusted, lanplcallfoid::regproc, laninline::regproc, lanvalidator::regproc, lanacl FROM pg_language WHERE lanname LIKE 'lang\\_%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanowner", REGROLE), Column("lanispl", BOOL), Column("lanpltrusted", BOOL), Column("lanplcallfoid", REGPROC), Column("laninline", REGPROC), Column("lanvalidator", REGPROC), Column("lanacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("lang_a"), T("postgres"), T("t"), T("f"), T("plpgsql_call_handler"), T("-"), T("-"), Null],
                            &[T("lang_b"), T("postgres"), T("t"), T("t"), T("plpgsql_call_handler"), T("plpgsql_inline_handler"), T("plpgsql_validator"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid), pg_describe_object(refclassid, refobjid, refobjsubid), deptype FROM pg_depend WHERE classid = 'pg_language'::regclass AND objid IN (SELECT oid FROM pg_language WHERE lanname LIKE 'lang\\_%') ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_describe_object", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("language lang_a"), T("function plpgsql_call_handler()"), T("n")],
                            &[T("language lang_b"), T("function plpgsql_call_handler()"), T("n")],
                            &[T("language lang_b"), T("function plpgsql_inline_handler(internal)"), T("n")],
                            &[T("language lang_b"), T("function plpgsql_validator(oid)"), T("n")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object('pg_language'::regclass, oid, 0)).*, (pg_identify_object_as_address('pg_language'::regclass, oid, 0)).object_names FROM pg_language WHERE lanname = 'lang_b';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT), Column("object_names", TEXT_ARRAY)],
                        rows: &[
                            &[T("language"), Null, T("lang_b"), T("lang_b"), T("{lang_b}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_bf(int) RETURNS int LANGUAGE lang_b AS 'BEGIN RETURN $1 * 2; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lang_bf(21);",
                    expected: Expected::Rows {
                        columns: &[Column("lang_bf", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_bbad(int) RETURNS int LANGUAGE lang_b AS 'BEGIN RETURN $1 * ; END';",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at end of input", position: 81, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_af(int) RETURNS int LANGUAGE lang_a AS 'BEGIN RETURN $1 + 100; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lang_af(1);",
                    expected: Expected::Rows {
                        columns: &[Column("lang_af", INT4)],
                        rows: &[
                            &[T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_abad(int) RETURNS int LANGUAGE lang_a AS 'BEGIN RETURN $1 * ; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO LANGUAGE lang_b 'BEGIN RAISE NOTICE ''in lang_b''; END';",
                    notices: &[Diagnostic { code: "00000", message: "in lang_b", ..N }],
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO LANGUAGE lang_a 'BEGIN RAISE NOTICE ''in lang_a''; END';",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "language \"lang_a\" does not support inline code execution", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE LANGUAGE lang_a HANDLER plpgsql_call_handler INLINE plpgsql_inline_handler;",
                    expected: Expected::Tag("CREATE LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO LANGUAGE lang_a 'BEGIN RAISE NOTICE ''in lang_a''; END';",
                    notices: &[Diagnostic { code: "00000", message: "in lang_a", ..N }],
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanpltrusted, laninline::regproc, lanvalidator::regproc FROM pg_language WHERE lanname = 'lang_a';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanpltrusted", BOOL), Column("laninline", REGPROC), Column("lanvalidator", REGPROC)],
                        rows: &[
                            &[T("lang_a"), T("f"), T("plpgsql_inline_handler"), T("-")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE lang_u1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE LANGUAGE lang_d HANDLER plpgsql_call_handler;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create custom procedural language", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_u1f() RETURNS int LANGUAGE lang_b AS 'BEGIN RETURN 1; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_u1g() RETURNS int LANGUAGE lang_a AS 'BEGIN RETURN 1; END';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language lang_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE lang_b TO lang_u2;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanacl FROM pg_language WHERE lanname = 'lang_b';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("lang_b"), T("{=U/postgres,postgres=U/postgres,lang_u2=U/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE lang_a OWNER TO lang_u1;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE lang_b OWNER TO lang_u2;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE lang_u1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE lang_a RENAME TO lang_b;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "language \"lang_b\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE lang_b RENAME TO lang_e;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of language lang_b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE lang_a RENAME TO lang_e;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE lang_b OWNER TO lang_u3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of language lang_b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE lang_e OWNER TO lang_u2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be able to SET ROLE \"lang_u2\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE lang_e OWNER TO lang_u3;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanowner::regrole, lanacl FROM pg_language WHERE lanname LIKE 'lang\\_%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanowner", REGROLE), Column("lanacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("lang_b"), T("lang_u2"), T("{=U/lang_u2,lang_u2=U/lang_u2}")],
                            &[T("lang_e"), T("lang_u3"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lang_af(2);",
                    expected: Expected::Rows {
                        columns: &[Column("lang_af", INT4)],
                        rows: &[
                            &[T("102")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid), pg_describe_object(refclassid, refobjid, 0), deptype FROM pg_shdepend WHERE classid = 'pg_language'::regclass AND dbid = (SELECT oid FROM pg_database WHERE datname = current_database()) ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_describe_object", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("language lang_b"), T("role lang_u2"), T("o")],
                            &[T("language lang_e"), T("role lang_u3"), T("o")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql RENAME TO plpgsql2;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql2 RENAME TO plpgsql;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE nosuch RENAME TO x;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "language \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP LANGUAGE lang_e;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop language lang_e because other objects depend on it", detail: "function lang_af(integer) depends on language lang_e\nfunction lang_abad(integer) depends on language lang_e", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP LANGUAGE lang_e CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 2 other objects", detail: "drop cascades to function lang_af(integer)\ndrop cascades to function lang_abad(integer)", ..N }],
                    expected: Expected::Tag("DROP LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP LANGUAGE IF EXISTS lang_e;",
                    notices: &[Diagnostic { code: "00000", message: "language \"lang_e\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP LANGUAGE lang_e;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "language \"lang_e\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname FROM pg_proc WHERE proname LIKE 'lang\\_%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME)],
                        rows: &[
                            &[T("lang_bf")],
                            &[T("lang_sqlf")],
                            &[T("lang_u1f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE LANGUAGE plpgsql HANDLER plpgsql_call_handler;",
                    expected: Expected::Tag("CREATE LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanpltrusted, laninline, lanvalidator FROM pg_language WHERE lanname = 'plpgsql';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanpltrusted", BOOL), Column("laninline", OID), Column("lanvalidator", OID)],
                        rows: &[
                            &[T("plpgsql"), T("f"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_pbad(int) RETURNS int LANGUAGE plpgsql AS 'BEGIN RETURN $1 * ; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO 'BEGIN RAISE NOTICE ''inline''; END';",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "language \"plpgsql\" does not support inline code execution", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE lang_u1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_pu() RETURNS int LANGUAGE plpgsql AS 'BEGIN RETURN 1; END';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language plpgsql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE TRUSTED LANGUAGE plpgsql HANDLER plpgsql_call_handler INLINE plpgsql_inline_handler VALIDATOR plpgsql_validator;",
                    expected: Expected::Tag("CREATE LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanpltrusted, laninline::regproc, lanvalidator::regproc FROM pg_language WHERE lanname = 'plpgsql';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanpltrusted", BOOL), Column("laninline", REGPROC), Column("lanvalidator", REGPROC)],
                        rows: &[
                            &[T("plpgsql"), T("t"), T("plpgsql_inline_handler"), T("plpgsql_validator")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE LANGUAGE sql HANDLER plpgsql_call_handler;",
                    expected: Expected::Tag("CREATE LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanispl, lanplcallfoid::regproc FROM pg_language WHERE lanname = 'sql';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanispl", BOOL), Column("lanplcallfoid", REGPROC)],
                        rows: &[
                            &[T("sql"), T("t"), T("plpgsql_call_handler")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lang_sqlf(41);",
                    expected: Expected::Rows {
                        columns: &[Column("lang_sqlf", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lang_sqlg(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 3';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lang_sqlg(5);",
                    expected: Expected::Rows {
                        columns: &[Column("lang_sqlg", INT4)],
                        rows: &[
                            &[T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP LANGUAGE lang_b CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 2 other objects", detail: "drop cascades to function lang_bf(integer)\ndrop cascades to function lang_u1f()", ..N }],
                    expected: Expected::Tag("DROP LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname FROM pg_language ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME)],
                        rows: &[
                            &[T("c")],
                            &[T("internal")],
                            &[T("plpgsql")],
                            &[T("sql")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
