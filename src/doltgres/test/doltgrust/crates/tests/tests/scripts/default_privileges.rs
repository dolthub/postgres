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
fn test_default_privileges() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER DEFAULT PRIVILEGES and the privileges new objects get",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE dp_owner LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE dp_reader;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE dp_other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO dp_owner;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA dp_s AUTHORIZATION dp_owner;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner GRANT SELECT ON TABLES TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner IN SCHEMA dp_s GRANT INSERT ON TABLES TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner REVOKE EXECUTE ON FUNCTIONS FROM PUBLIC;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner GRANT USAGE ON SEQUENCES TO dp_reader WITH GRANT OPTION;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner GRANT USAGE ON TYPES TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner GRANT USAGE ON SCHEMAS TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_userbyid(defaclrole) AS role, defaclnamespace::regnamespace AS nsp, defaclobjtype, defaclacl FROM pg_default_acl ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("role", NAME), Column("nsp", REGNAMESPACE), Column("defaclobjtype", CHAR), Column("defaclacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dp_owner"), T("-"), T("S"), T("{dp_owner=rwU/dp_owner,dp_reader=U*/dp_owner}")],
                            &[T("dp_owner"), T("-"), T("T"), T("{=U/dp_owner,dp_owner=U/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_owner"), T("-"), T("f"), T("{dp_owner=X/dp_owner}")],
                            &[T("dp_owner"), T("-"), T("n"), T("{dp_owner=UC/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_owner"), T("-"), T("r"), T("{dp_owner=arwdDxtm/dp_owner,dp_reader=r/dp_owner}")],
                            &[T("dp_owner"), T("dp_s"), T("r"), T("{dp_reader=a/dp_owner}")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE dp_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dp_t (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dp_s.t (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE dp_seq;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION dp_f() RETURNS INT LANGUAGE SQL AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE dp_ty AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN dp_dom AS INT;",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA dp_s2 AUTHORIZATION dp_owner;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relacl FROM pg_class WHERE relname IN ('dp_t', 't', 'dp_seq') AND relnamespace IN ('public'::regnamespace, 'dp_s'::regnamespace) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dp_seq"), T("{dp_owner=rwU/dp_owner,dp_reader=U*/dp_owner}")],
                            &[T("dp_t"), T("{dp_owner=arwdDxtm/dp_owner,dp_reader=r/dp_owner}")],
                            &[T("t"), T("{dp_owner=arwdDxtm/dp_owner,dp_reader=ar/dp_owner}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, proacl FROM pg_proc WHERE proname = 'dp_f';",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("proacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dp_f"), T("{dp_owner=X/dp_owner}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typname, typacl FROM pg_type WHERE typname IN ('dp_ty', 'dp_dom') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("typacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dp_dom"), T("{=U/dp_owner,dp_owner=U/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_ty"), T("{=U/dp_owner,dp_owner=U/dp_owner,dp_reader=U/dp_owner}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspname, nspacl FROM pg_namespace WHERE nspname = 'dp_s2';",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME), Column("nspacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dp_s2"), T("{dp_owner=UC/dp_owner,dp_reader=U/dp_owner}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('dp_reader', 'dp_t', 'SELECT'), has_table_privilege('dp_reader', 'dp_s.t', 'INSERT'), has_function_privilege('dp_other', 'dp_f()', 'EXECUTE');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("has_table_privilege", BOOL), Column("has_function_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner REVOKE SELECT ON TABLES FROM dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner GRANT EXECUTE ON FUNCTIONS TO PUBLIC;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner REVOKE GRANT OPTION FOR USAGE ON SEQUENCES FROM dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_userbyid(defaclrole) AS role, defaclnamespace::regnamespace AS nsp, defaclobjtype, defaclacl FROM pg_default_acl ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("role", NAME), Column("nsp", REGNAMESPACE), Column("defaclobjtype", CHAR), Column("defaclacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dp_owner"), T("-"), T("S"), T("{dp_owner=rwU/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_owner"), T("-"), T("T"), T("{=U/dp_owner,dp_owner=U/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_owner"), T("-"), T("n"), T("{dp_owner=UC/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_owner"), T("dp_s"), T("r"), T("{dp_reader=a/dp_owner}")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES IN SCHEMA dp_s GRANT USAGE ON SCHEMAS TO dp_reader;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "cannot use IN SCHEMA clause when using GRANT/REVOKE ON SCHEMAS", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_missing GRANT SELECT ON TABLES TO dp_reader;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "dp_missing" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES IN SCHEMA dp_missing GRANT SELECT ON TABLES TO dp_reader;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: r#"schema "dp_missing" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES GRANT SELECT ON TABLES TO dp_missing;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "dp_missing" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES GRANT DELETE ON SEQUENCES TO dp_reader;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type DELETE for sequence", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES GRANT EXECUTE ON ROUTINES TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE dp_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner GRANT SELECT ON TABLES TO dp_other;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to change default privileges", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES GRANT SELECT ON TABLES TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_userbyid(defaclrole) AS role, defaclnamespace::regnamespace AS nsp, defaclobjtype, defaclacl FROM pg_default_acl ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("role", NAME), Column("nsp", REGNAMESPACE), Column("defaclobjtype", CHAR), Column("defaclacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dp_other"), T("-"), T("r"), T("{dp_reader=r/dp_other,dp_other=arwdDxtm/dp_other}")],
                            &[T("dp_owner"), T("-"), T("S"), T("{dp_owner=rwU/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_owner"), T("-"), T("T"), T("{=U/dp_owner,dp_owner=U/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_owner"), T("-"), T("n"), T("{dp_owner=UC/dp_owner,dp_reader=U/dp_owner}")],
                            &[T("dp_owner"), T("dp_s"), T("r"), T("{dp_reader=a/dp_owner}")],
                            &[T("postgres"), T("-"), T("f"), T("{=X/postgres,postgres=X/postgres,dp_reader=X/postgres}")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner IN SCHEMA dp_s REVOKE INSERT ON TABLES FROM dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_default_acl WHERE defaclnamespace = 'dp_s'::regnamespace;",
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
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_owner IN SCHEMA dp_s GRANT USAGE ON TYPES TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_default_acl'::regclass, oid, 0) AS description, (pg_identify_object('pg_default_acl'::regclass, oid, 0)).identity FROM pg_default_acl ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("description", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("default privileges on new functions belonging to role postgres"), T("for role postgres on functions")],
                            &[T("default privileges on new relations belonging to role dp_other"), T("for role dp_other on tables")],
                            &[T("default privileges on new schemas belonging to role dp_owner"), T("for role dp_owner on schemas")],
                            &[T("default privileges on new sequences belonging to role dp_owner"), T("for role dp_owner on sequences")],
                            &[T("default privileges on new types belonging to role dp_owner"), T("for role dp_owner on types")],
                            &[T("default privileges on new types belonging to role dp_owner in schema dp_s"), T("for role dp_owner in schema dp_s on types")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object_as_address('pg_default_acl'::regclass, oid, 0)).* FROM pg_default_acl ORDER BY 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("object_names", TEXT_ARRAY), Column("object_args", TEXT_ARRAY)],
                        rows: &[
                            &[T("default acl"), T("{dp_other}"), T("{r}")],
                            &[T("default acl"), T("{dp_owner}"), T("{S}")],
                            &[T("default acl"), T("{dp_owner}"), T("{T}")],
                            &[T("default acl"), T("{dp_owner}"), T("{n}")],
                            &[T("default acl"), T("{dp_owner,dp_s}"), T("{T}")],
                            &[T("default acl"), T("{postgres}"), T("{f}")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION dp_evt() RETURNS event_trigger LANGUAGE plpgsql AS $$DECLARE r RECORD; BEGIN FOR r IN SELECT * FROM pg_event_trigger_ddl_commands() LOOP RAISE NOTICE '% % %', r.command_tag, r.object_type, r.object_identity; END LOOP; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER dp_et ON ddl_command_end EXECUTE FUNCTION dp_evt();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES GRANT USAGE ON SCHEMAS TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    notices: &[Diagnostic { code: "00000", message: "ALTER DEFAULT PRIVILEGES SCHEMAS <NULL>", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES REVOKE USAGE ON SCHEMAS FROM dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    notices: &[Diagnostic { code: "00000", message: "ALTER DEFAULT PRIVILEGES SCHEMAS <NULL>", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER dp_et;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA dp_s CASCADE;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to table dp_s.t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) FROM pg_depend WHERE classid = 'pg_default_acl'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE dp_renamed;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_renamed GRANT SELECT ON TABLES TO dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE dp_renamed RENAME TO dp_renamed2;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dp_rt (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE dp_rt OWNER TO dp_renamed2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO dp_renamed2;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE dp_renamed2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dp_rt2 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relacl FROM pg_class WHERE relname IN ('dp_rt', 'dp_rt2') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("dp_rt"), Null],
                            &[T("dp_rt2"), T("{dp_reader=r/dp_renamed2,dp_renamed2=arwdDxtm/dp_renamed2}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE dp_renamed2 REVOKE SELECT ON TABLES FROM dp_reader;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_default_acl WHERE defaclrole = 'dp_renamed2'::regrole;",
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
