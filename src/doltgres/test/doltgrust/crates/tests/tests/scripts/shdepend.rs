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
fn test_shared_dependencies() {
    run_scripts(&[
        ScriptTest {
            name: "pg_shdepend, DROP ROLE, DROP OWNED, and REASSIGN OWNED",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE sd_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sd_grantee;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sd_new;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sd_admin CREATEROLE;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO sd_owner;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sd_s AUTHORIZATION sd_owner;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sd_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sd_t (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sd_s.t2 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE sd_seq;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sd_f() RETURNS INT LANGUAGE SQL AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE sd_ty AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW sd_v AS SELECT 1 AS x;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON sd_t TO sd_grantee;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sd_other (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT, INSERT ON sd_other TO sd_grantee;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE (a) ON sd_other TO sd_grantee;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY sd_p ON sd_other TO sd_grantee USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES FOR ROLE sd_owner GRANT SELECT ON TABLES TO sd_grantee;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass, objid <> 0 AS has_objid, objsubid, refclassid::regclass, pg_get_userbyid(refobjid) AS role, deptype FROM pg_shdepend WHERE refobjid IN ('sd_owner'::regrole, 'sd_grantee'::regrole) AND dbid <> 0 ORDER BY 5, 1, 6, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("classid", REGCLASS), Column("has_objid", BOOL), Column("objsubid", INT4), Column("refclassid", REGCLASS), Column("role", NAME), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_default_acl"), T("t"), T("0"), T("pg_authid"), T("sd_grantee"), T("a")],
                            &[T("pg_class"), T("t"), T("0"), T("pg_authid"), T("sd_grantee"), T("a")],
                            &[T("pg_class"), T("t"), T("0"), T("pg_authid"), T("sd_grantee"), T("a")],
                            &[T("pg_class"), T("t"), T("1"), T("pg_authid"), T("sd_grantee"), T("a")],
                            &[T("pg_policy"), T("t"), T("0"), T("pg_authid"), T("sd_grantee"), T("r")],
                            &[T("pg_default_acl"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("o")],
                            &[T("pg_type"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("o")],
                            &[T("pg_proc"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("o")],
                            &[T("pg_class"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("o")],
                            &[T("pg_class"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("o")],
                            &[T("pg_class"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("o")],
                            &[T("pg_class"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("o")],
                            &[T("pg_namespace"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("a")],
                            &[T("pg_namespace"), T("t"), T("0"), T("pg_authid"), T("sd_owner"), T("o")],
                        ],
                        tag: "SELECT 14",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) AS description, deptype FROM pg_shdepend WHERE refobjid = 'sd_owner'::regrole ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("description", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("default privileges on new relations belonging to role sd_owner"), T("o")],
                            &[T("function sd_f()"), T("o")],
                            &[T("schema public"), T("a")],
                            &[T("schema sd_s"), T("o")],
                            &[T("sequence sd_seq"), T("o")],
                            &[T("table sd_s.t2"), T("o")],
                            &[T("table sd_t"), T("o")],
                            &[T("type sd_ty"), T("o")],
                            &[T("view sd_v"), T("o")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) AS description, deptype FROM pg_shdepend WHERE refobjid = 'sd_grantee'::regrole ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("description", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("column a of table sd_other"), T("a")],
                            &[T("default privileges on new relations belonging to role sd_owner"), T("a")],
                            &[T("policy sd_p on table sd_other"), T("r")],
                            &[T("table sd_other"), T("a")],
                            &[T("table sd_t"), T("a")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_owner;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "sd_owner" cannot be dropped because some objects depend on it"#, detail: "privileges for schema public
owner of schema sd_s
owner of table sd_t
owner of table sd_s.t2
owner of sequence sd_seq
owner of function sd_f()
owner of type sd_ty
owner of view sd_v
owner of default privileges on new relations belonging to role sd_owner", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_grantee;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "sd_grantee" cannot be dropped because some objects depend on it"#, detail: "privileges for table sd_t
privileges for table sd_other
privileges for column a of table sd_other
target of policy sd_p on table sd_other
privileges for default privileges on new relations belonging to role sd_owner", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sd_admin;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY sd_owner;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to drop objects", detail: r#"Only roles with privileges of role "sd_owner" may drop objects owned by it."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY sd_owner TO sd_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to reassign objects", detail: r#"Only roles with privileges of role "sd_owner" may reassign objects owned by it."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY sd_owner TO sd_new;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, pg_get_userbyid(relowner) AS owner, relacl FROM pg_class WHERE relname IN ('sd_t', 't2', 'sd_seq', 'sd_v') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("owner", NAME), Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("sd_seq"), T("sd_new"), Null],
                            &[T("sd_t"), T("sd_new"), T("{sd_new=arwdDxtm/sd_new,sd_grantee=r/sd_new}")],
                            &[T("sd_v"), T("sd_new"), Null],
                            &[T("t2"), T("sd_new"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspname, pg_get_userbyid(nspowner) AS owner FROM pg_namespace WHERE nspname = 'sd_s';",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME), Column("owner", NAME)],
                        rows: &[
                            &[T("sd_s"), T("sd_new")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, pg_get_userbyid(proowner) AS owner FROM pg_proc WHERE proname = 'sd_f';",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("owner", NAME)],
                        rows: &[
                            &[T("sd_f"), T("sd_new")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typname, pg_get_userbyid(typowner) AS owner FROM pg_type WHERE typname = 'sd_ty';",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("owner", NAME)],
                        rows: &[
                            &[T("sd_ty"), T("sd_new")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_default_acl WHERE defaclrole = 'sd_owner'::regrole;",
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
                    query: "DROP ROLE sd_owner;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "sd_owner" cannot be dropped because some objects depend on it"#, detail: "privileges for schema public
owner of default privileges on new relations belonging to role sd_owner", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY sd_grantee;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relacl FROM pg_class WHERE relname IN ('sd_t', 'sd_other') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("sd_other"), T("{postgres=arwdDxtm/postgres}")],
                            &[T("sd_t"), T("{sd_new=arwdDxtm/sd_new}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attacl FROM pg_attribute WHERE attrelid = 'sd_other'::regclass AND attnum > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("a"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT polname FROM pg_policy WHERE polrelid = 'sd_other'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("polname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_grantee;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sd_two;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sd_three;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sd_pt (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY sd_p2 ON sd_pt TO sd_two, sd_three USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY sd_two;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT polname, polroles::regrole[] FROM pg_policy WHERE polrelid = 'sd_pt'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("polname", NAME), Column("polroles", REGROLE_ARRAY)],
                        rows: &[
                            &[T("sd_p2"), T("{sd_three}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sd_drop;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sd_dt (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sd_dt OWNER TO sd_drop;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW sd_dv AS SELECT * FROM sd_dt;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY sd_drop;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table sd_dt because other objects depend on it", detail: "view sd_dv depends on table sd_dt", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY sd_drop CASCADE;",
                    expected: Expected::Tag("DROP OWNED"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to view sd_dv", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname IN ('sd_dt', 'sd_dv');",
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
                    query: "DROP ROLE sd_drop;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY postgres;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop objects owned by role postgres because they are required by the database system", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY postgres TO sd_new;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot reassign ownership of objects owned by role postgres because they are required by the database system", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY sd_new TO sd_missing;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "sd_missing" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY sd_missing;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "sd_missing" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_shdepend's rows for each kind of object",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE sv_owner SUPERUSER;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sv_user;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sv_member;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sv_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sv_s;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SCHEMA sv_s TO sv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sv_s.t (a INT PRIMARY KEY, b TEXT, c SERIAL, d INT GENERATED ALWAYS AS IDENTITY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX sv_i ON sv_s.t (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT (b), UPDATE (b) ON sv_s.t TO sv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW sv_s.v AS SELECT a FROM sv_s.t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW sv_s.mv AS SELECT a FROM sv_s.t;",
                    expected: Expected::Tag("SELECT 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sv_s.p (a INT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sv_s.p1 PARTITION OF sv_s.p FOR VALUES FROM (1) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sv_s.big (x TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE sv_s.seq;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SEQUENCE sv_s.seq TO sv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE sv_s.comp AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE sv_s.en AS ENUM ('a');",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE sv_s.rng AS RANGE (SUBTYPE = INT4);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN sv_s.dom AS INT;",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON DOMAIN sv_s.dom TO sv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sv_s.f(INT) RETURNS INT LANGUAGE SQL AS 'SELECT $1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION sv_s.f(INT) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION sv_s.f(INT) TO sv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE sv_s.agg (INT) (SFUNC = int4pl, STYPE = INT);",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE sv_s.pr() LANGUAGE SQL AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR sv_s.### (LEFTARG = INT, RIGHTARG = INT, FUNCTION = int4pl);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR FAMILY sv_s.fam USING btree;",
                    expected: Expected::Tag("CREATE OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS sv_s.cls FOR TYPE INT4 USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 btint4cmp(INT4, INT4);",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION sv_s.coll (LOCALE = 'C', PROVIDER = builtin);",
                    expected: Expected::Tag("CREATE COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS sv_s.st ON a, b FROM sv_s.t;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sv_s.evt() RETURNS event_trigger LANGUAGE plpgsql AS 'BEGIN END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER sv_et ON ddl_command_end EXECUTE FUNCTION sv_s.evt();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY sv_pol ON sv_s.t TO sv_user, sv_member USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DEFAULT PRIVILEGES IN SCHEMA sv_s GRANT SELECT ON TABLES TO sv_user;",
                    expected: Expected::Tag("ALTER DEFAULT PRIVILEGES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT sv_member TO sv_user WITH ADMIN OPTION;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sv_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT sv_member TO sv_owner GRANTED BY sv_user;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql OWNER TO sv_owner;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE plpgsql TO sv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE postgres OWNER TO sv_owner;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA public OWNER TO sv_owner;",
                    expected: Expected::Tag("ALTER SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION pg_catalog.now() TO sv_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dbid = 0 AS shared, classid::regclass AS catalog, pg_describe_object(classid, objid, objsubid) AS description, pg_get_userbyid(refobjid) AS role, deptype FROM pg_shdepend ORDER BY 3, 4, 5;",
                    expected: Expected::Rows {
                        columns: &[Column("shared", BOOL), Column("catalog", REGCLASS), Column("description", TEXT), Column("role", NAME), Column("deptype", CHAR)],
                        rows: &[
                            &[T("f"), T("pg_collation"), T("collation sv_s.coll"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_class"), T("column b of table sv_s.t"), T("sv_user"), T("a")],
                            &[T("t"), T("pg_database"), T("database postgres"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_default_acl"), T("default privileges on new relations belonging to role sv_owner in schema sv_s"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_default_acl"), T("default privileges on new relations belonging to role sv_owner in schema sv_s"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_event_trigger"), T("event trigger sv_et"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_proc"), T("function now()"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_proc"), T("function sv_s.agg(integer)"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_proc"), T("function sv_s.evt()"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_proc"), T("function sv_s.f(integer)"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_proc"), T("function sv_s.f(integer)"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_proc"), T("function sv_s.pr()"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_language"), T("language plpgsql"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_language"), T("language plpgsql"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_class"), T("materialized view sv_s.mv"), T("sv_owner"), T("o")],
                            &[T("t"), T("pg_auth_members"), T("membership of role sv_owner in role sv_member"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_opclass"), T("operator class sv_s.cls for access method btree"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_opfamily"), T("operator family sv_s.cls for access method btree"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_opfamily"), T("operator family sv_s.fam for access method btree"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_operator"), T("operator sv_s.###(integer,integer)"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_policy"), T("policy sv_pol on table sv_s.t"), T("sv_member"), T("r")],
                            &[T("f"), T("pg_policy"), T("policy sv_pol on table sv_s.t"), T("sv_user"), T("r")],
                            &[T("f"), T("pg_namespace"), T("schema public"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_namespace"), T("schema sv_s"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_namespace"), T("schema sv_s"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_class"), T("sequence sv_s.seq"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_class"), T("sequence sv_s.seq"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_class"), T("sequence sv_s.t_c_seq"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_class"), T("sequence sv_s.t_d_seq"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_statistic_ext"), T("statistics object sv_s.st"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_class"), T("table sv_s.big"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_class"), T("table sv_s.p"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_class"), T("table sv_s.p1"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_class"), T("table sv_s.t"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_type"), T("type sv_s.comp"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_type"), T("type sv_s.dom"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_type"), T("type sv_s.dom"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_type"), T("type sv_s.en"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_type"), T("type sv_s.rng"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_class"), T("view sv_s.v"), T("sv_owner"), T("o")],
                        ],
                        tag: "SELECT 40",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sv_user;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "sv_user" cannot be dropped because some objects depend on it"#, detail: "privileges for function now()
privileges for language plpgsql
privileges for schema sv_s
privileges for column b of table sv_s.t
privileges for sequence sv_s.seq
privileges for type sv_s.dom
privileges for function sv_s.f(integer)
target of policy sv_pol on table sv_s.t
privileges for default privileges on new relations belonging to role sv_owner in schema sv_s
privileges for membership of role sv_owner in role sv_member", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER sv_et;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE postgres OWNER TO postgres;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA public OWNER TO pg_database_owner;",
                    expected: Expected::Tag("ALTER SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql OWNER TO postgres;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON LANGUAGE plpgsql FROM sv_user;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION pg_catalog.now() FROM sv_user;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY sv_owner TO sv_member;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dbid = 0 AS shared, classid::regclass AS catalog, pg_describe_object(classid, objid, objsubid) AS description, pg_get_userbyid(refobjid) AS role, deptype FROM pg_shdepend ORDER BY 3, 4, 5;",
                    expected: Expected::Rows {
                        columns: &[Column("shared", BOOL), Column("catalog", REGCLASS), Column("description", TEXT), Column("role", NAME), Column("deptype", CHAR)],
                        rows: &[
                            &[T("f"), T("pg_collation"), T("collation sv_s.coll"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("column b of table sv_s.t"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_default_acl"), T("default privileges on new relations belonging to role sv_owner in schema sv_s"), T("sv_owner"), T("o")],
                            &[T("f"), T("pg_default_acl"), T("default privileges on new relations belonging to role sv_owner in schema sv_s"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_proc"), T("function sv_s.agg(integer)"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_proc"), T("function sv_s.evt()"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_proc"), T("function sv_s.f(integer)"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_proc"), T("function sv_s.f(integer)"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_proc"), T("function sv_s.pr()"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("materialized view sv_s.mv"), T("sv_member"), T("o")],
                            &[T("t"), T("pg_auth_members"), T("membership of role sv_owner in role sv_member"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_opclass"), T("operator class sv_s.cls for access method btree"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_opfamily"), T("operator family sv_s.cls for access method btree"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_opfamily"), T("operator family sv_s.fam for access method btree"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_operator"), T("operator sv_s.###(integer,integer)"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_policy"), T("policy sv_pol on table sv_s.t"), T("sv_member"), T("r")],
                            &[T("f"), T("pg_policy"), T("policy sv_pol on table sv_s.t"), T("sv_user"), T("r")],
                            &[T("f"), T("pg_namespace"), T("schema sv_s"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_namespace"), T("schema sv_s"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_class"), T("sequence sv_s.seq"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("sequence sv_s.seq"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_class"), T("sequence sv_s.t_c_seq"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("sequence sv_s.t_d_seq"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_statistic_ext"), T("statistics object sv_s.st"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("table sv_s.big"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("table sv_s.p"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("table sv_s.p1"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("table sv_s.t"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_type"), T("type sv_s.comp"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_type"), T("type sv_s.dom"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_type"), T("type sv_s.dom"), T("sv_user"), T("a")],
                            &[T("f"), T("pg_type"), T("type sv_s.en"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_type"), T("type sv_s.rng"), T("sv_member"), T("o")],
                            &[T("f"), T("pg_class"), T("view sv_s.v"), T("sv_member"), T("o")],
                        ],
                        tag: "SELECT 34",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY sv_user, sv_member CASCADE;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dbid = 0 AS shared, classid::regclass AS catalog, pg_describe_object(classid, objid, objsubid) AS description, pg_get_userbyid(refobjid) AS role, deptype FROM pg_shdepend ORDER BY 3, 4, 5;",
                    expected: Expected::Rows {
                        columns: &[Column("shared", BOOL), Column("catalog", REGCLASS), Column("description", TEXT), Column("role", NAME), Column("deptype", CHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sv_user;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sv_member;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sv_owner;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an extension's owner in pg_shdepend, DROP ROLE, and REASSIGN OWNED",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE r5x SUPERUSER;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE r5x;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION \"uuid-ossp\";",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r5x;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "r5x" cannot be dropped because some objects depend on it"#, detail: "owner of extension uuid-ossp
owner of function uuid_nil()
owner of function uuid_ns_dns()
owner of function uuid_ns_url()
owner of function uuid_ns_oid()
owner of function uuid_ns_x500()
owner of function uuid_generate_v1()
owner of function uuid_generate_v1mc()
owner of function uuid_generate_v3(uuid,text)
owner of function uuid_generate_v4()
owner of function uuid_generate_v5(uuid,text)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass, deptype, count(*) FROM pg_shdepend WHERE refobjid = 'r5x'::regrole GROUP BY 1, 2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("classid", REGCLASS), Column("deptype", CHAR), Column("count", INT8)],
                        rows: &[
                            &[T("pg_proc"), T("o"), T("10")],
                            &[T("pg_extension"), T("o"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY r5x TO postgres;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.extname, r.rolname FROM pg_extension e JOIN pg_roles r ON r.oid = e.extowner WHERE e.extname = 'uuid-ossp';",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("rolname", NAME)],
                        rows: &[
                            &[T("uuid-ossp"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r5x;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_drop_owned_after_waiting_for_a_concurrent_drop_owned() {
    run_scripts(&[
        ScriptTest {
            name: "a DROP OWNED that waited for another one to revoke the same grants finds nothing left to revoke",
            set_up_script: &[
                "CREATE ROLE regress_dropowned_role;",
                "CREATE ROLE regress_dropowned_member;",
                "CREATE ROLE regress_dropowned_grantor;",
                "GRANT regress_dropowned_role TO regress_dropowned_grantor WITH ADMIN OPTION;",
                "SET ROLE regress_dropowned_grantor;",
                "GRANT regress_dropowned_role TO regress_dropowned_member;",
                "RESET ROLE;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_dropowned_grantor;",
                    expected: Expected::Tag("DROP OWNED"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_dropowned_grantor;",
                    expected: Expected::Tag("DROP OWNED"),
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_auth_members WHERE grantor = 'regress_dropowned_grantor'::regrole;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
