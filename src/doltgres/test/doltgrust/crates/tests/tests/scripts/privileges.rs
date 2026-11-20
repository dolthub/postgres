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
use harness::script::{A, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_privilege_inquiry_functions() {
    run_scripts(&[
        ScriptTest {
            name: "has_*_privilege, pg_has_role, pg_get_acl, and the template databases' ACLs",
            set_up_script: &[
                "CREATE TABLE t (a INT, b TEXT);",
                "ALTER TABLE t DROP COLUMN b;",
                "CREATE SEQUENCE sq;",
                "CREATE SCHEMA s;",
                "CREATE FUNCTION f(INT) RETURNS INT AS 'SELECT 1' LANGUAGE sql;",
                "CREATE TYPE ty AS (x INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('t', 'SELECT'), has_table_privilege('postgres', 't', 'select, insert'), has_table_privilege('pg_signal_backend', 't', 'SELECT'), has_table_privilege('public', 't', 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('pg_signal_backend', 'pg_class', 'SELECT'), has_table_privilege('pg_signal_backend', 'pg_class', 'UPDATE'), has_table_privilege('pg_signal_backend', 'pg_class'::regclass, 'SELECT WITH GRANT OPTION');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege(99999::oid, 'SELECT'), has_table_privilege('pg_signal_backend', 99999::oid, 'SELECT'), has_table_privilege(10::oid, 99999::oid, 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL)],
                        rows: &[
                            &[Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('nosuch', 'SELECT');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('nosuchrole', 't', 'SELECT');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuchrole" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('t', 'FOO');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "FOO""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('t', ' select , MAINTAIN WITH GRANT OPTION ');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('t', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: """#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_sequence_privilege('sq', 'USAGE'), has_sequence_privilege('pg_signal_backend', 'sq', 'USAGE'), has_sequence_privilege('sq'::regclass, 'UPDATE'), has_sequence_privilege(99999::oid, 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_sequence_privilege", BOOL), Column("has_sequence_privilege", BOOL), Column("has_sequence_privilege", BOOL), Column("has_sequence_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_sequence_privilege('t', 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""t" is not a sequence"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_sequence_privilege('t'::regclass, 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""t" is not a sequence"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_sequence_privilege('t', 'DELETE');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "DELETE""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_sequence_privilege('nosuch', 'FOO');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "FOO""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_any_column_privilege('t', 'SELECT'), has_any_column_privilege('pg_signal_backend', 't', 'INSERT'), has_any_column_privilege(99999::oid, 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_any_column_privilege", BOOL), Column("has_any_column_privilege", BOOL), Column("has_any_column_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_any_column_privilege('t', 'DELETE');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "DELETE""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_column_privilege('t', 'a', 'SELECT'), has_column_privilege('pg_signal_backend', 't', 'a', 'SELECT'), has_column_privilege('t', 1::int2, 'UPDATE'), has_column_privilege('t', 2::int2, 'SELECT'), has_column_privilege('t', 9::int2, 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_column_privilege", BOOL), Column("has_column_privilege", BOOL), Column("has_column_privilege", BOOL), Column("has_column_privilege", BOOL), Column("has_column_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_column_privilege('t', 'ctid', 'SELECT'), has_column_privilege('pg_signal_backend', 't', 'tableoid', 'SELECT'), has_column_privilege('t', -1::int2, 'SELECT'), has_column_privilege(99999::oid, 'a', 'SELECT'), has_column_privilege(99999::oid, 1::int2, 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_column_privilege", BOOL), Column("has_column_privilege", BOOL), Column("has_column_privilege", BOOL), Column("has_column_privilege", BOOL), Column("has_column_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_column_privilege('t', 'nosuch', 'SELECT');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nosuch" of relation "t" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_column_privilege('t', '........pg.dropped.2........', 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_column_privilege", BOOL)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_column_privilege('t', 'a', 'TRUNCATE');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "TRUNCATE""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_database_privilege('postgres', 'CONNECT'), has_database_privilege('pg_signal_backend', 'postgres', 'CONNECT, CREATE'), has_database_privilege('pg_signal_backend', 'postgres', 'CREATE'), has_database_privilege('pg_signal_backend', 'template1', 'TEMP'), has_database_privilege(99999::oid, 'CONNECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_database_privilege", BOOL), Column("has_database_privilege", BOOL), Column("has_database_privilege", BOOL), Column("has_database_privilege", BOOL), Column("has_database_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("f"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_database_privilege('nosuch', 'CONNECT');",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: r#"database "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_foreign_data_wrapper_privilege(99999::oid, 'USAGE'), has_server_privilege(99999::oid, 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_foreign_data_wrapper_privilege", BOOL), Column("has_server_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_foreign_data_wrapper_privilege('nosuch', 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"foreign-data wrapper "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_server_privilege('pg_signal_backend', 'nosuch', 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"server "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_function_privilege('f(int)', 'EXECUTE'), has_function_privilege('pg_signal_backend', 'f(int)', 'EXECUTE'), has_function_privilege('pg_signal_backend', 'abs(int4)', 'execute'), has_function_privilege(99999::oid, 'EXECUTE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_function_privilege", BOOL), Column("has_function_privilege", BOOL), Column("has_function_privilege", BOOL), Column("has_function_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_function_privilege('nosuch(int)', 'EXECUTE');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"function "nosuch(int)" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_function_privilege('-', 'EXECUTE');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"function "-" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_function_privilege('f', 'EXECUTE');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "expected a left parenthesis", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_language_privilege('sql', 'USAGE'), has_language_privilege('pg_signal_backend', 'sql', 'USAGE'), has_language_privilege('pg_signal_backend', 'c', 'USAGE'), has_language_privilege('pg_signal_backend', 'internal', 'USAGE'), has_language_privilege('pg_signal_backend', 'plpgsql', 'USAGE'), has_language_privilege(99999::oid, 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_language_privilege", BOOL), Column("has_language_privilege", BOOL), Column("has_language_privilege", BOOL), Column("has_language_privilege", BOOL), Column("has_language_privilege", BOOL), Column("has_language_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_language_privilege('nosuch', 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"language "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_schema_privilege('s', 'CREATE'), has_schema_privilege('pg_signal_backend', 's', 'USAGE'), has_schema_privilege('pg_signal_backend', 'public', 'CREATE'), has_schema_privilege('pg_signal_backend', 'public', 'USAGE'), has_schema_privilege('pg_signal_backend', 'pg_catalog', 'USAGE'), has_schema_privilege(99999::oid, 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_schema_privilege", BOOL), Column("has_schema_privilege", BOOL), Column("has_schema_privilege", BOOL), Column("has_schema_privilege", BOOL), Column("has_schema_privilege", BOOL), Column("has_schema_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("f"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_schema_privilege('nosuch', 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: r#"schema "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_tablespace_privilege('pg_default', 'CREATE'), has_tablespace_privilege('pg_signal_backend', 'pg_default', 'CREATE'), has_tablespace_privilege('pg_signal_backend', 'pg_global', 'CREATE'), has_tablespace_privilege(99999::oid, 'CREATE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_tablespace_privilege", BOOL), Column("has_tablespace_privilege", BOOL), Column("has_tablespace_privilege", BOOL), Column("has_tablespace_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_tablespace_privilege('nosuch', 'CREATE');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"tablespace "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_type_privilege('ty', 'USAGE'), has_type_privilege('pg_signal_backend', 'ty', 'USAGE'), has_type_privilege('pg_signal_backend', 'int4[]', 'USAGE'), has_type_privilege('pg_signal_backend', 'int4multirange', 'USAGE'), has_type_privilege(99999::oid, 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_type_privilege", BOOL), Column("has_type_privilege", BOOL), Column("has_type_privilege", BOOL), Column("has_type_privilege", BOOL), Column("has_type_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_type_privilege('nosuch', 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_type_privilege('-', 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "-" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_parameter_privilege('work_mem', 'SET'), has_parameter_privilege('pg_signal_backend', 'work_mem', 'SET, ALTER SYSTEM'), has_parameter_privilege(10::oid, 'nosuch.thing', 'ALTER SYSTEM');",
                    expected: Expected::Rows {
                        columns: &[Column("has_parameter_privilege", BOOL), Column("has_parameter_privilege", BOOL), Column("has_parameter_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_parameter_privilege('work_mem', 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "USAGE""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_largeobject_privilege(12345::oid, 'SELECT'), has_largeobject_privilege('pg_signal_backend', 12345::oid, 'UPDATE'), has_largeobject_privilege(10::oid, 12345::oid, 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_largeobject_privilege", BOOL), Column("has_largeobject_privilege", BOOL), Column("has_largeobject_privilege", BOOL)],
                        rows: &[
                            &[Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_largeobject_privilege(12345::oid, 'USAGE');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "USAGE""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_has_role('pg_monitor', 'pg_read_all_stats', 'MEMBER'), pg_has_role('pg_monitor', 'pg_read_all_stats', 'USAGE'), pg_has_role('pg_monitor', 'pg_read_all_stats', 'SET'), pg_has_role('pg_monitor', 'pg_read_all_stats', 'MEMBER WITH ADMIN OPTION'), pg_has_role('pg_read_all_stats', 'pg_monitor', 'MEMBER'), pg_has_role('pg_monitor', 'pg_signal_backend', 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_has_role", BOOL), Column("pg_has_role", BOOL), Column("pg_has_role", BOOL), Column("pg_has_role", BOOL), Column("pg_has_role", BOOL), Column("pg_has_role", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("f"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_has_role('pg_signal_backend', 'MEMBER'), pg_has_role('pg_signal_backend', 'pg_monitor', 'USAGE'), pg_has_role('pg_monitor'::regrole, 'pg_monitor'::regrole, 'USAGE WITH GRANT OPTION'), pg_has_role(99999::oid, 'USAGE'), pg_has_role('pg_monitor', 'pg_database_owner', 'MEMBER'), pg_has_role('postgres', 'pg_database_owner', 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_has_role", BOOL), Column("pg_has_role", BOOL), Column("pg_has_role", BOOL), Column("pg_has_role", BOOL), Column("pg_has_role", BOOL), Column("pg_has_role", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("f"), T("t"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_has_role('public', 'pg_signal_backend', 'MEMBER');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "public" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_has_role('pg_signal_backend', 'nosuch', 'MEMBER');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_has_role('pg_signal_backend', 'pg_monitor', 'CREATE');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "CREATE""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('pg_read_all_data', 't', 'SELECT'), has_table_privilege('pg_read_all_data', 't', 'INSERT'), has_table_privilege('pg_write_all_data', 't', 'INSERT, SELECT'), has_table_privilege('pg_write_all_data', 't', 'TRUNCATE'), has_table_privilege('pg_maintain', 't', 'MAINTAIN'), has_table_privilege('pg_write_all_data', 'pg_class', 'UPDATE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), T("f"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_schema_privilege('pg_read_all_data', 's', 'USAGE'), has_schema_privilege('pg_write_all_data', 's', 'CREATE'), has_sequence_privilege('pg_read_all_data', 'sq', 'SELECT'), has_column_privilege('pg_read_all_data', 't', 'a', 'SELECT'), has_any_column_privilege('pg_write_all_data', 't', 'UPDATE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_schema_privilege", BOOL), Column("has_schema_privilege", BOOL), Column("has_sequence_privilege", BOOL), Column("has_column_privilege", BOOL), Column("has_any_column_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_database_privilege('pg_database_owner', 'postgres', 'CREATE'), pg_has_role('postgres', 'pg_database_owner', 'MEMBER'), has_table_privilege('pg_database_owner', 't', 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_database_privilege", BOOL), Column("pg_has_role", BOOL), Column("has_table_privilege", BOOL)],
                        rows: &[
                            &[T("f"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_acl('pg_class'::regclass, 'pg_authid'::regclass, 0), pg_get_acl('pg_class'::regclass, 't'::regclass, 0), pg_get_acl('pg_class'::regclass, 't'::regclass, 1), pg_get_acl('pg_class'::regclass, 't'::regclass, 2), pg_get_acl('pg_class'::regclass, 99999, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_acl", ACLITEM_ARRAY), Column("pg_get_acl", ACLITEM_ARRAY), Column("pg_get_acl", ACLITEM_ARRAY), Column("pg_get_acl", ACLITEM_ARRAY), Column("pg_get_acl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres}"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_acl('pg_proc'::regclass, 'pg_read_file(text)'::regprocedure, 0), pg_get_acl('pg_proc'::regclass, 'abs(int4)'::regprocedure, 0), pg_get_acl('pg_namespace'::regclass, 'public'::regnamespace, 0), pg_get_acl('pg_database'::regclass, 1, 0), pg_get_acl('pg_database'::regclass, 5, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_acl", ACLITEM_ARRAY), Column("pg_get_acl", ACLITEM_ARRAY), Column("pg_get_acl", ACLITEM_ARRAY), Column("pg_get_acl", ACLITEM_ARRAY), Column("pg_get_acl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=X/postgres}"), Null, T("{pg_database_owner=UC/pg_database_owner,=U/pg_database_owner}"), T("{=c/postgres,postgres=CTc/postgres}"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_acl(0, 0, 0), pg_get_acl('pg_language'::regclass, 12, 0), pg_get_acl('pg_type'::regclass, 23, 0), pg_get_acl('pg_tablespace'::regclass, 1663, 0), pg_get_acl('pg_am'::regclass, 403, 0), pg_get_acl(12345, 1, 0);",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "unrecognized class ID: 12345", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (aclexplode(pg_get_acl('pg_database'::regclass, 1, 0))).*;",
                    expected: Expected::Rows {
                        columns: &[Column("grantor", OID), Column("grantee", OID), Column("privilege_type", TEXT), Column("is_grantable", BOOL)],
                        rows: &[
                            &[T("10"), T("0"), T("CONNECT"), T("f")],
                            &[T("10"), T("10"), T("CREATE"), T("f")],
                            &[T("10"), T("10"), T("TEMPORARY"), T("f")],
                            &[T("10"), T("10"), T("CONNECT"), T("f")],
                        ],
                        tag: "SELECT 4",
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
                    query: "CREATE DATABASE d2 TEMPLATE template1;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, datacl FROM pg_database WHERE datname = 'd2';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("datacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("d2"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_database_privilege('pg_signal_backend', 'template0', 'CONNECT'), has_database_privilege('pg_signal_backend', 'template1', 'CONNECT, TEMP'), has_database_privilege('pg_signal_backend', 'template1', 'TEMP');",
                    expected: Expected::Rows {
                        columns: &[Column("has_database_privilege", BOOL), Column("has_database_privilege", BOOL), Column("has_database_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pg_signal_backend;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('t', 'SELECT'), has_schema_privilege('public', 'CREATE'), pg_has_role('pg_signal_backend', 'USAGE'), has_function_privilege('f(int)', 'EXECUTE'), has_type_privilege('ty', 'USAGE'), has_parameter_privilege('work_mem', 'SET');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("has_schema_privilege", BOOL), Column("pg_has_role", BOOL), Column("has_function_privilege", BOOL), Column("has_type_privilege", BOOL), Column("has_parameter_privilege", BOOL)],
                        rows: &[
                            &[T("f"), T("f"), T("t"), T("t"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_grant_and_revoke() {
    run_scripts(&[
        ScriptTest {
            name: "GRANT and REVOKE on tables, columns, sequences, views, functions, procedures, schemas, domains, and types, with PG18's warnings and errors",
            set_up_script: &[
                "CREATE SCHEMA gs;",
                "CREATE TABLE gs.t (a INT, b TEXT, c INT);",
                "CREATE VIEW gs.v AS SELECT a FROM gs.t;",
                "CREATE SEQUENCE gs.s;",
                "CREATE FUNCTION gs.f(x INT) RETURNS INT LANGUAGE sql AS 'SELECT x';",
                "CREATE PROCEDURE gs.p() LANGUAGE sql AS 'SELECT 1';",
                "CREATE DOMAIN gs.d AS INT;",
                "CREATE TYPE gs.e AS ENUM ('x', 'y');",
                "CREATE INDEX t_a ON gs.t (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relname, relacl FROM pg_class WHERE relnamespace = 'gs'::regnamespace ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("s"), Null],
                            &[T("t"), Null],
                            &[T("t_a"), Null],
                            &[T("v"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT, INSERT ON gs.t TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON gs.t TO pg_read_all_data WITH GRANT OPTION;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE (b), SELECT (a, c) ON gs.t TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'gs.t'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,pg_monitor=ar/postgres,pg_read_all_data=a*r*w*d*D*x*t*m*/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attacl FROM pg_attribute WHERE attrelid = 'gs.t'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("a"), T("{=r/postgres}")],
                            &[T("b"), T("{=w/postgres}")],
                            &[T("c"), T("{=r/postgres}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON gs.t FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attacl FROM pg_attribute WHERE attrelid = 'gs.t'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("a"), Null],
                            &[T("b"), T("{=w/postgres}")],
                            &[T("c"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE GRANT OPTION FOR INSERT ON gs.t FROM pg_read_all_data;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'gs.t'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,pg_monitor=ar/postgres,pg_read_all_data=ar*w*d*D*x*t*m*/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALL ON gs.t FROM pg_monitor;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'gs.t'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,pg_read_all_data=ar*w*d*D*x*t*m*/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON gs.t TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type USAGE for table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON gs.t_a TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""t_a" is an index"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON gs.t TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type EXECUTE for relation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON SEQUENCE gs.t TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""t" is not a sequence"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE, DELETE ON gs.s TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "0LP01", message: r#"sequence "s" only supports USAGE, SELECT, and UPDATE privileges"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON SEQUENCE gs.s TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE (last_value) ON gs.s TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "0LP01", message: r#"sequence "s" only supports SELECT column privileges"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'gs.s'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=rwU/postgres,pg_monitor=U/postgres,=rwU/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT (nope) ON gs.t TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nope" of relation "t" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT DELETE (a) ON gs.t TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type DELETE for column", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT (a) ON SEQUENCE gs.s TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "column privileges are only valid for relations", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON gs.v TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'gs.v'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,pg_monitor=r/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION gs.f(int) TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION gs.f(int) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALL ON PROCEDURE gs.p() FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, proacl FROM pg_proc WHERE pronamespace = 'gs'::regnamespace ORDER BY proname;",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("proacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("f"), T("{postgres=X/postgres,pg_monitor=X/postgres}")],
                            &[T("p"), T("{postgres=X/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SCHEMA gs TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA gs TO pg_monitor WITH GRANT OPTION;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspacl FROM pg_namespace WHERE nspname = 'gs';",
                    expected: Expected::Rows {
                        columns: &[Column("nspacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=UC/postgres,=U/postgres,pg_monitor=C*/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON DOMAIN gs.d TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON TYPE gs.e FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typname, typacl FROM pg_type WHERE typnamespace = 'gs'::regnamespace AND typtype IN ('d', 'e') ORDER BY typname;",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("typacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("d"), T("{=U/postgres,postgres=U/postgres,pg_monitor=U/postgres}")],
                            &[T("e"), T("{postgres=U/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON TYPE gs._e TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "cannot set privileges of array types", hint: "Set the privileges of the element type instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON DOMAIN gs.e TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""gs.e" is not a domain"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON gs.t TO PUBLIC WITH GRANT OPTION;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "grant options can only be granted to roles", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON gs.t TO nosuchrole;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuchrole" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT FOO ON gs.t TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized privilege type "foo""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON gs.t TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type CREATE for relation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SEQUENCE gs.s TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type CREATE for sequence", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON SCHEMA gs TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type EXECUTE for schema", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON gs.nosuch TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "gs.nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION gs.nosuch() TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function gs.nosuch() does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SCHEMA nosuch TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: r#"schema "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE nosuch TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"language "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE c TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"language "c" is not trusted"#, detail: "GRANT and REVOKE are not allowed on untrusted languages, because only superusers can use untrusted languages.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON FOREIGN DATA WRAPPER nosuch TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"foreign-data wrapper "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON FOREIGN SERVER nosuch TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"server "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON LARGE OBJECT 1234 TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "large object 1234 does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CONNECT ON DATABASE nosuch TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: r#"database "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON gs.t TO PUBLIC GRANTED BY pg_monitor;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "grantor must be current user", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON gs.t TO pg_monitor GRANTED BY CURRENT_USER;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALL ON ALL TABLES IN SCHEMA gs FROM pg_read_all_data CASCADE;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA gs TO pg_write_all_data;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON ALL SEQUENCES IN SCHEMA gs TO pg_write_all_data;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA gs TO pg_write_all_data;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON ALL PROCEDURES IN SCHEMA gs TO pg_write_all_data;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relacl FROM pg_class WHERE relnamespace = 'gs'::regnamespace ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("s"), T("{postgres=rwU/postgres,pg_monitor=U/postgres,=rwU/postgres,pg_write_all_data=U/postgres}")],
                            &[T("t"), T("{postgres=arwdDxtm/postgres,pg_monitor=r/postgres,pg_write_all_data=r/postgres}")],
                            &[T("t_a"), Null],
                            &[T("v"), T("{postgres=arwdDxtm/postgres,pg_monitor=r/postgres,pg_write_all_data=r/postgres}")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, proacl FROM pg_proc WHERE pronamespace = 'gs'::regnamespace ORDER BY proname;",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("proacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("f"), T("{postgres=X/postgres,pg_monitor=X/postgres,pg_write_all_data=X/postgres}")],
                            &[T("p"), T("{postgres=X/postgres,pg_write_all_data=X/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('pg_write_all_data', 'gs.t', 'SELECT'), has_column_privilege('public', 'gs.t', 'b', 'UPDATE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("has_column_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "GRANT and REVOKE as event triggers report them, in a rolled-back transaction, and on built-in objects",
            set_up_script: &[
                "CREATE TABLE et (a INT);",
                "CREATE FUNCTION et_report() RETURNS event_trigger LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
  FOR r IN SELECT * FROM pg_event_trigger_ddl_commands() LOOP
    RAISE NOTICE 'ddl: % % % % % %', r.classid IS NULL, r.objid IS NULL, r.command_tag, r.object_type, r.schema_name IS NULL, r.object_identity IS NULL;
  END LOOP;
END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER et_end ON ddl_command_end EXECUTE FUNCTION et_report();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON et TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { code: "00000", message: "ddl: t t GRANT TABLE t t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON et FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    notices: &[Diagnostic { code: "00000", message: "ddl: t t REVOKE TABLE t t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION et_report() TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { code: "00000", message: "ddl: t t GRANT FUNCTION t t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SCHEMA public TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { code: "00000", message: "ddl: t t GRANT SCHEMA t t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER et_end;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON et TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'et'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,pg_monitor=r/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'et'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CONNECT ON DATABASE postgres TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON TABLESPACE pg_default TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SET ON PARAMETER work_mem TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION abs(int) TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE plpgsql TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON TYPE int4 TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON pg_class TO pg_monitor;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_privileges_and_comments_of_builtin_objects() {
    run_scripts(&[
        ScriptTest {
            name: "GRANT, REVOKE, and COMMENT change built-in objects' rows",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'pg_class'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,=r/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON pg_catalog.pg_class FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'pg_class'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('public', 'pg_class', 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON pg_catalog.pg_class TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'pg_class'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,=r/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE (relname) ON pg_catalog.pg_class TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attacl FROM pg_attribute WHERE attrelid = 'pg_class'::regclass AND attname = 'relname';",
                    expected: Expected::Rows {
                        columns: &[Column("attacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{=w/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attacl FROM pg_attribute WHERE attrelid = 'pg_class'::regclass AND attname = 'relnamespace';",
                    expected: Expected::Rows {
                        columns: &[Column("attacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE UPDATE (relname) ON pg_catalog.pg_class FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attacl FROM pg_attribute WHERE attrelid = 'pg_class'::regclass AND attname = 'relname';",
                    expected: Expected::Rows {
                        columns: &[Column("attacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION now() FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proacl FROM pg_proc WHERE oid = 'now'::regproc;",
                    expected: Expected::Rows {
                        columns: &[Column("proacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=X/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION now() TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proacl FROM pg_proc WHERE oid = 'now'::regproc;",
                    expected: Expected::Rows {
                        columns: &[Column("proacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=X/postgres,=X/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CREATE ON SCHEMA public FROM pg_database_owner;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspacl FROM pg_namespace WHERE nspname = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("nspacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{pg_database_owner=U/pg_database_owner,=U/pg_database_owner}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO pg_database_owner;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspacl FROM pg_namespace WHERE nspname = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("nspacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{pg_database_owner=UC/pg_database_owner,=U/pg_database_owner}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON TYPE int4 FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typacl FROM pg_type WHERE oid = 'int4'::regtype;",
                    expected: Expected::Rows {
                        columns: &[Column("typacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=U/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_type_privilege('public', 'int4', 'USAGE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_type_privilege", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON TYPE int4 TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typacl FROM pg_type WHERE oid = 'int4'::regtype;",
                    expected: Expected::Rows {
                        columns: &[Column("typacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=U/postgres,=U/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('now'::regproc, 'pg_proc');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("current transaction time")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON FUNCTION now() IS 'my now';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('now'::regproc, 'pg_proc');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("my now")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON FUNCTION now() IS NULL;",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('now'::regproc, 'pg_proc');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON FUNCTION now() IS 'again';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('now'::regproc, 'pg_proc');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("again")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_description WHERE objoid = 'now'::regproc;",
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
                    query: "COMMENT ON TABLE pg_class IS 'the relations';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN pg_class.relname IS 'the name';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('pg_class'::regclass, 'pg_class'), col_description('pg_class'::regclass, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT), Column("col_description", TEXT)],
                        rows: &[
                            &[T("the relations"), T("the name")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objsubid, description FROM pg_description WHERE objoid = 'pg_class'::regclass ORDER BY objsubid;",
                    expected: Expected::Rows {
                        columns: &[Column("objsubid", INT4), Column("description", TEXT)],
                        rows: &[
                            &[T("0"), T("the relations")],
                            &[T("2"), T("the name")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE pg_class IS NULL;",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objsubid, description FROM pg_description WHERE objoid = 'pg_class'::regclass ORDER BY objsubid;",
                    expected: Expected::Rows {
                        columns: &[Column("objsubid", INT4), Column("description", TEXT)],
                        rows: &[
                            &[T("2"), T("the name")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN pg_class.relname IS NULL;",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_description WHERE objoid = 'pg_class'::regclass;",
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
                    query: "COMMENT ON SCHEMA public IS 'mine';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('public'::regnamespace, 'pg_namespace');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("mine")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON pg_catalog.pg_class FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON FUNCTION now() IS 'in a transaction';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE oid = 'pg_class'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,=r/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('now'::regproc, 'pg_proc');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("again")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER and OWNER TO change built-in objects' rows",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER FUNCTION now() COST 2;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT procost FROM pg_proc WHERE oid = 'now'::regproc;",
                    expected: Expected::Rows {
                        columns: &[Column("procost", FLOAT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA public OWNER TO postgres;",
                    expected: Expected::Tag("ALTER SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspowner::regrole FROM pg_namespace WHERE nspname = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("nspowner", REGROLE)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pg_catalog.pg_class OWNER TO postgres;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied: "pg_class" is a system catalog"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a GRANT on a built-in function waits for a transaction that locked its pg_proc row",
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT oid FROM pg_proc WHERE oid = 'now()'::regprocedure FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID)],
                        rows: &[
                            &[T("1299")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ REVOKE EXECUTE ON FUNCTION now() FROM PUBLIC",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT proacl FROM pg_proc WHERE oid = 'now()'::regprocedure",
                    expected: Expected::Rows {
                        columns: &[Column("proacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=X/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT EXECUTE ON FUNCTION now() TO PUBLIC",
                    expected: Expected::Tag("GRANT"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a COMMENT on a built-in object waits for a transaction that locked its pg_description row",
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT description FROM pg_description WHERE objoid = 'now()'::regprocedure FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("description", TEXT)],
                        rows: &[
                            &[T("current transaction time")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMENT ON FUNCTION now() IS 'my now'",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT obj_description('now()'::regprocedure, 'pg_proc')",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("my now")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMENT ON FUNCTION now() IS 'current transaction time'",
                    expected: Expected::Tag("COMMENT"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_privileges_are_enforced() {
    run_scripts(&[
        ScriptTest {
            name: "table, column, function, and index function privileges are enforced",
            set_up_script: &[
                "CREATE ROLE pr_user;",
                "CREATE ROLE pr_owner;",
                "CREATE ROLE pr_bypass BYPASSRLS;",
                "GRANT CREATE ON SCHEMA public TO pr_owner;",
                "SET ROLE pr_owner;",
                "CREATE TABLE pt (a INT PRIMARY KEY, b INT, c TEXT);",
                "INSERT INTO pt VALUES (1, 10, 'one'), (2, 20, 'two');",
                "CREATE VIEW pv AS SELECT a, b FROM pt;",
                "CREATE VIEW pv_inv WITH (security_invoker) AS SELECT a, b FROM pt;",
                "RESET ROLE;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM pt;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (3, 30, 'three');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET b = 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pt;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pv;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for view pv", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT (a, b) ON pt TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON pv, pv_inv TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c FROM pt;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pv ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pv_inv ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT (b) ON pt FROM pr_user;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pv_inv ORDER BY a;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pv ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT INSERT (a, b) ON pt TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE (b) ON pt TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt (a, b) VALUES (3, 30);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt (a, b, c) VALUES (4, 40, 'four');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt (a, b) VALUES (5, 50) RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt (a, b) VALUES (6, 60) RETURNING c;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET b = b + 1 WHERE a = 3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET c = 'x' WHERE a = 3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET b = 1 WHERE c = 'one';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt (a, b) VALUES (3, 33) ON CONFLICT (a) DO UPDATE SET b = excluded.b;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt (a, b) VALUES (3, 33) ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pt WHERE a = 3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO pt USING (VALUES (3, 300)) AS s(a, b) ON pt.a = s.a WHEN MATCHED THEN UPDATE SET b = s.b;",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO pt USING (VALUES (7, 700)) AS s(a, b) ON pt.a = s.a WHEN NOT MATCHED THEN INSERT (a, b) VALUES (s.a, s.b);",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO pt USING (VALUES (3, 300)) AS s(a, b) ON pt.a = s.a WHEN MATCHED THEN DELETE;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM pt WHERE a = 3 FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("one")],
                            &[T("2"), T("20"), T("two")],
                            &[T("3"), T("300"), Null],
                            &[T("5"), T("50"), Null],
                            &[T("7"), T("700"), Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pf(x INT) RETURNS INT LANGUAGE sql AS 'SELECT x + 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pf_plpgsql(x INT) RETURNS INT LANGUAGE plpgsql AS 'BEGIN RETURN x + 1; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION psrf(n INT) RETURNS SETOF INT LANGUAGE sql AS 'SELECT generate_series(1, n)';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pplus(INT, INT) RETURNS INT LANGUAGE sql AS 'SELECT $1 + $2';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR ### (LEFTARG = INT, RIGHTARG = INT, FUNCTION = pplus);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE psum(INT) (SFUNC = int4pl, STYPE = INT, INITCOND = '0');",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION pf(INT), pf_plpgsql(INT), psrf(INT), pplus(INT, INT) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION psum(INT) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pf(1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function pf", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pf_plpgsql(1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function pf_plpgsql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM psrf(2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function psrf", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT psrf(2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function psrf", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 ### 2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function pplus", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT psum(x) FROM (VALUES (1), (2)) v(x);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for aggregate psum", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT psum(x) OVER () FROM (VALUES (1), (2)) v(x);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function psum", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 WHERE false AND pf(1) = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION psum(INT) TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT psum(x) FROM (VALUES (1), (2)) v(x);",
                    expected: Expected::Rows {
                        columns: &[Column("psum", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pidx (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pidx_expr ON pidx ((pf(a)));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT INSERT, SELECT, UPDATE ON pidx TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pidx SELECT 1 WHERE false;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pidx VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function pf", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pidx VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pidx SET a = 2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function pf", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX pidx_expr;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pidx_part ON pidx (a) WHERE pf(a) > 0;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pidx VALUES (3);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function pf", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pk_t (id INT PRIMARY KEY, secret TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pk_t VALUES (1, 's');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fk_t (id INT REFERENCES pk_t, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT INSERT, UPDATE ON fk_t TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT (secret) ON pk_t TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk_t VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk_t VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "fk_t" violates foreign key constraint "fk_t_id_fkey""#, detail: r#"Key is not present in table "pk_t"."#, schema: "public", table: "fk_t", constraint: "fk_t_id_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON fk_t TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk_t VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "fk_t" violates foreign key constraint "fk_t_id_fkey""#, detail: r#"Key (id)=(2) is not present in table "pk_t"."#, schema: "public", table: "fk_t", constraint: "fk_t_id_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY pt TO STDOUT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY pt (a, b) TO STDOUT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY pt TO '/tmp/x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to COPY to a file", detail: r#"Only roles with privileges of the "pg_write_server_files" role may COPY to a file."#, hint: r#"Anyone can COPY to stdout or from stdin. psql's \copy command also works for anyone."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY pt FROM PROGRAM 'echo';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to COPY to or from an external program", detail: r#"Only roles with privileges of the "pg_execute_server_program" role may COPY to or from an external program."#, hint: r#"Anyone can COPY to stdout or from stdin. psql's \copy command also works for anyone."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE pt IN ACCESS SHARE MODE;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE pt IN ROW EXCLUSIVE MODE;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE pt IN ACCESS EXCLUSIVE MODE;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE pk_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pk_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE pt;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pctas AS SELECT 1 AS x;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT TRUNCATE ON pt TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE pt;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prls (a INT, owner_name TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO prls VALUES (1, 'x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE prls ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON prls TO pr_user, pr_bypass;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prls;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("owner_name", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prls;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("owner_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prls;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "prls""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET row_security;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_bypass;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prls;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("owner_name", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prls;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("owner_name", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fpk (id INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fpk VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fpk ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fpk FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ffk (id INT REFERENCES fpk);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT INSERT ON ffk TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ffk VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ffk VALUES (2);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "ffk" violates foreign key constraint "ffk_id_fkey""#, detail: r#"Key (id)=(2) is not present in table "fpk"."#, schema: "public", table: "ffk", constraint: "ffk_id_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fpk;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ffk VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ffk VALUES (3);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "ffk" violates foreign key constraint "ffk_id_fkey""#, detail: r#"Key is not present in table "fpk"."#, schema: "public", table: "ffk", constraint: "ffk_id_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pidx2 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pidx2_expr ON pidx2 ((pf(a)));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT INSERT, SELECT ON pidx2 TO pr_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pidx2 SET a = a;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pidx2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pidx2;",
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
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE own_t (x INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO own_t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM own_t;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relowner::regrole FROM pg_class WHERE relname = 'own_t';",
                    expected: Expected::Rows {
                        columns: &[Column("relowner", REGROLE)],
                        rows: &[
                            &[T("pr_owner")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE pr_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY pidx2 FROM STDIN;",
                    expected: Expected::Tag("COPY 0"),
                    copy_from_stdin_file: "copy-to-empty.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY pidx2 FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function pf", context: Some("COPY pidx2, line 1"), ..E }),
                    copy_from_stdin_file: "privileges-index-function.tsv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY pt (c) FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table pt", ..E }),
                    copy_from_stdin_file: "privileges-index-function.tsv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_toast_tables() {
    run_scripts(&[
        ScriptTest {
            name: "system catalogs' TOAST tables are empty and refuse changes",
            set_up_script: &[
                "CREATE ROLE toast_reader;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE pg_toast.pg_toast_1213 SET chunk_id = 1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"cannot change TOAST relation "pg_toast_1213""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pg_toast.pg_toast_2619;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"cannot change TOAST relation "pg_toast_2619""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pg_toast.pg_toast_2619 VALUES (1, 1, '\\x00');",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"cannot change TOAST relation "pg_toast_2619""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_toast.pg_toast_2619;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("catalog TOAST tables hold no values, since no value is stored out of line"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_toast.pg_toast_2619 WHERE chunk_id = 5 AND chunk_seq = 0;",
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
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_toast.pg_toast_2619 WHERE chunk_id = 5 AND chunk_seq = 0;",
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
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relkind, relpages FROM pg_class WHERE relnamespace = 'pg_toast'::REGNAMESPACE AND relname IN ('pg_toast_2619', 'pg_toast_2619_index') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relkind", CHAR), Column("relpages", INT4)],
                        rows: &[
                            &[T("pg_toast_2619"), T("t"), T("3")],
                            &[T("pg_toast_2619_index"), T("i"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE toast_reader;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pg_toast.pg_toast_1213 SET chunk_id = 1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema pg_toast", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_toast.pg_toast_1213;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema pg_toast", position: 22, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_owner_of_built_in_objects_of_every_kind() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER ... OWNER TO changes the owners of built-in dictionaries, collations, conversions, operator classes and families, and configurations",
            set_up_script: &["CREATE ROLE r6;"],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH DICTIONARY simple OWNER TO r6;",
                    expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION \"C\" OWNER TO r6;",
                    expected: Expected::Tag("ALTER COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER CONVERSION pg_catalog.utf8_to_iso_8859_1 OWNER TO r6;",
                    expected: Expected::Tag("ALTER CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR CLASS int4_ops USING btree OWNER TO r6;",
                    expected: Expected::Tag("ALTER OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR FAMILY integer_ops USING btree OWNER TO r6;",
                    expected: Expected::Tag("ALTER OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION english OWNER TO r6;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT dictowner::regrole::text FROM pg_ts_dict WHERE dictname = 'simple') AS dict, (SELECT collowner::regrole::text FROM pg_collation WHERE collname = 'C') AS coll, (SELECT conowner::regrole::text FROM pg_conversion WHERE conname = 'utf8_to_iso_8859_1') AS conv, (SELECT opcowner::regrole::text FROM pg_opclass WHERE opcname = 'int4_ops' AND opcmethod = 403) AS opc, (SELECT opfowner::regrole::text FROM pg_opfamily WHERE opfname = 'integer_ops' AND opfmethod = 403) AS opf, (SELECT cfgowner::regrole::text FROM pg_ts_config WHERE cfgname = 'english') AS cfg;",
                    expected: Expected::Rows {
                        columns: &[Column("dict", TEXT), Column("coll", TEXT), Column("conv", TEXT), Column("opc", TEXT), Column("opf", TEXT), Column("cfg", TEXT)],
                        rows: &[
                            &[T("r6"), T("r6"), T("r6"), T("r6"), T("r6"), T("r6")],
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
fn test_grants_and_owner_changes_behind_a_concurrent_change() {
    run_scripts(&[
        ScriptTest {
            name: "GRANT on a table waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE TABLE gt (c INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP TABLE gt",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT SELECT ON gt TO r1",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"gt\" does not exist", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "GRANT on a function waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE FUNCTION gf() RETURNS INT LANGUAGE sql AS 'SELECT 1'",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP FUNCTION gf()",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT EXECUTE ON FUNCTION gf() TO r1",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function gf() does not exist", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "GRANT on a schema waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE SCHEMA gs",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP SCHEMA gs",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT USAGE ON SCHEMA gs TO r1",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: "schema \"gs\" does not exist", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "GRANT on a type waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE TYPE gty AS (a INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP TYPE gty",
                    expected: Expected::Tag("DROP TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT USAGE ON TYPE gty TO r1",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"gty\" does not exist", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "GRANT on a column waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE TABLE gc (c INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP TABLE gc",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT SELECT (c) ON gc TO r1",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"gc\" does not exist", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "GRANT on a sequence waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE SEQUENCE gq",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP SEQUENCE gq",
                    expected: Expected::Tag("DROP SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT USAGE ON SEQUENCE gq TO r1",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"gq\" does not exist", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER FUNCTION OWNER waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE FUNCTION go() RETURNS INT LANGUAGE sql AS 'SELECT 1'",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP FUNCTION go()",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER FUNCTION go() OWNER TO r1",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function go() does not exist", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE OWNER waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE TABLE gto (c INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP TABLE gto",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE gto OWNER TO r1",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"gto\" does not exist", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER SCHEMA OWNER waiting for a concurrent DROP fails as Postgres does once the DROP commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE SCHEMA gso",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DROP SCHEMA gso",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER SCHEMA gso OWNER TO r1",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tuple concurrently deleted", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER SCHEMA OWNER waiting for a concurrent GRANT fails as Postgres does once the GRANT commits",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE SCHEMA gsu",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ GRANT USAGE ON SCHEMA gsu TO r1",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER SCHEMA gsu OWNER TO r1",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tuple concurrently updated", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            skip: Some("Appendix C: a catalog write behind a committed update of its row reruns"),
            ..S
        },
        ScriptTest {
            name: "a GRANT on a built-in table behind a lock of its row, which a REVOKE put back as it was built in, rereads the row once the lock ends",
            set_up_script: &[
                "CREATE ROLE r1",
                "CREATE ROLE r2",
                "GRANT SELECT ON pg_class TO r1",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ REVOKE SELECT ON pg_class FROM r1",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN ISOLATION LEVEL READ COMMITTED",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT relname FROM pg_class WHERE oid = 'pg_class'::regclass FOR UPDATE",
                    flow: Flow::Exec,
                    client: "C",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT UPDATE ON pg_class TO r2",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT relacl FROM pg_class WHERE oid = 'pg_class'::regclass",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,=r/postgres,r2=w/postgres}")],
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
            name: "a REVOKE behind a lock of a pg_class row that a committed DELETE removed fails its lookup by OID",
            set_up_script: &["CREATE TABLE igi (c INT)"],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM pg_class WHERE relname = 'igi'",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN ISOLATION LEVEL READ COMMITTED",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT relhasindex FROM pg_class WHERE oid = 'igi'::regclass FOR UPDATE",
                    flow: Flow::Exec,
                    client: "C",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ REVOKE SELECT ON igi FROM PUBLIC",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "cache lookup failed for relation ", message_contains: true, ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_built_in_objects_keep_their_initial_privileges() {
    run_scripts(&[
        ScriptTest {
            name: "a fresh database's built-in objects have the ACLs pg_init_privs records, which pg_dump leaves out",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class c LEFT JOIN pg_init_privs p ON p.objoid = c.oid AND p.classoid = 'pg_class'::regclass AND p.objsubid = 0 WHERE c.relnamespace = 'pg_catalog'::regnamespace AND c.relacl IS DISTINCT FROM p.initprivs;",
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
                    query: "SELECT count(*) FROM pg_proc f LEFT JOIN pg_init_privs p ON p.objoid = f.oid AND p.classoid = 'pg_proc'::regclass AND p.objsubid = 0 WHERE f.pronamespace = 'pg_catalog'::regnamespace AND f.proacl IS DISTINCT FROM p.initprivs;",
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
                    query: "SELECT count(*) FROM pg_namespace n LEFT JOIN pg_init_privs p ON p.objoid = n.oid AND p.classoid = 'pg_namespace'::regclass AND p.objsubid = 0 WHERE n.nspname NOT IN ('public', 'information_schema') AND n.nspacl IS DISTINCT FROM p.initprivs;",
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
