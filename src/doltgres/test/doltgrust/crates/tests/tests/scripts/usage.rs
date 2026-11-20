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
fn test_usage_privileges() {
    run_scripts(&[
        ScriptTest {
            name: "Schema USAGE in search paths and qualified names",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE us_user LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA us_s;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE us_s.t (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON us_s.t TO us_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION us_s.f() RETURNS INT LANGUAGE SQL AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE us_s.ty AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA us_user;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE us_user.mine (m INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON us_user.mine TO us_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA us_open;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SCHEMA us_open TO us_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE us_open.t (c INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON us_open.t TO us_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET search_path = us_s, "$user", us_open, public;"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE us_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW search_path;",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T(r#"us_s, "$user", us_open, public"#)],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{us_open,public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schema();",
                    expected: Expected::Rows {
                        columns: &[Column("current_schema", NAME)],
                        rows: &[
                            &[T("us_open")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("c", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mine;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "mine" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM us_s.t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema us_s", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT us_s.f();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema us_s", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function f() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT NULL::us_s.ty IS NULL AS isnull;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema us_s", position: 14, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM us_s.missing;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema us_s", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM us_missing.t;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "us_missing.t" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 OPERATOR(us_s.+) 1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema us_s", position: 10, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'us_s.t'::regclass;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema us_s", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE us_s.new (a INT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema us_s", position: 14, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA us_s TO us_user;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema us_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SCHEMA us_user TO us_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE us_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{us_user,us_open,public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mine;",
                    expected: Expected::Rows {
                        columns: &[Column("m", INT4)],
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
                    query: "GRANT USAGE ON SCHEMA us_s TO us_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE us_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{us_s,us_user,us_open,public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT us_s.f();",
                    expected: Expected::Rows {
                        columns: &[Column("f", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT NULL::us_s.ty IS NULL AS isnull;",
                    expected: Expected::Rows {
                        columns: &[Column("isnull", BOOL)],
                        rows: &[
                            &[T("t")],
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
                    query: "REVOKE USAGE ON SCHEMA us_s FROM us_user;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE us_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{us_user,us_open,public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("c", INT4)],
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
                    query: "REVOKE USAGE ON SCHEMA public FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE us_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{us_user,us_open}")],
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
                    query: "GRANT pg_database_owner TO us_user;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"role "pg_database_owner" cannot have explicit members"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE postgres OWNER TO us_user;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE us_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{us_user,us_open,public}")],
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
                    query: "ALTER DATABASE postgres OWNER TO postgres;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SCHEMA public TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE us_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE us_tmp (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_temp.us_tmp;",
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
                    query: "SELECT current_schemas(true) = current_schemas(true);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
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
                    query: "GRANT pg_read_all_data TO us_user;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE us_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{us_s,us_user,us_open,public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
            ],
            ..S
        },
        ScriptTest {
            name: "Type, language, function, sequence, and setting privileges",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE ru_user LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ru_ty AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ru_dom AS INT;",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ru_en AS ENUM ('a', 'b');",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_f(INT, INT) RETURNS INT LANGUAGE SQL IMMUTABLE AS 'SELECT $1 + $2';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_trig() RETURNS TRIGGER LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE ru_proc() LANGUAGE SQL AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_diff(FLOAT8, FLOAT8) RETURNS FLOAT8 LANGUAGE SQL IMMUTABLE AS 'SELECT $1 - $2';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_sfunc(INT, INT) RETURNS INT LANGUAGE SQL IMMUTABLE AS 'SELECT $1 + $2';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE ru_seq;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_t (a INT, b INT GENERATED ALWAYS AS IDENTITY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON ru_t TO ru_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON TYPE ru_ty FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON DOMAIN ru_dom FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON TYPE ru_en FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION ru_f(INT, INT) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION ru_trig() FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON PROCEDURE ru_proc() FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION ru_diff(FLOAT8, FLOAT8) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION ru_sfunc(INT, INT) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION eqsel(INTERNAL, OID, INTERNAL, INTEGER) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO ru_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_a(ru_ty) RETURNS INT LANGUAGE SQL AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_ty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_b() RETURNS ru_ty LANGUAGE SQL AS 'SELECT NULL::ru_ty';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_ty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_c(ru_ty[]) RETURNS INT LANGUAGE SQL AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_ty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_d() RETURNS INT LANGUAGE SQL BEGIN ATOMIC SELECT NULL::ru_en IS NULL; END;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_en", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_e(a INT DEFAULT NULL::ru_dom) RETURNS INT LANGUAGE SQL AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_t1 (a ru_ty);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_ty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_t2 (a ru_dom[]);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_t3 (a INT DEFAULT NULL::ru_dom);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_t4 (a INT CHECK (a::ru_dom > 0));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_t5 (a INT) PARTITION BY RANGE ((a::ru_dom));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_t6 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON ru_t6 ((a::ru_dom));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type public.ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON ru_t6 (a) WHERE a::ru_dom > 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type public.ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ru_st ON (a::ru_dom), (a + 1) FROM ru_t6;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ru_t6 ALTER COLUMN a TYPE ru_dom;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ru_t6 ALTER COLUMN a SET DEFAULT NULL::ru_dom;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ru_t6 ADD CHECK (a::ru_dom > 0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ru_t6 ADD COLUMN b ru_en;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_en", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW ru_v AS SELECT NULL::ru_en AS e;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_en", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY ru_p ON ru_t6 USING (a::ru_dom > 0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER ru_tr BEFORE INSERT ON ru_t6 FOR EACH ROW WHEN (NEW.a::ru_dom > 0) EXECUTE FUNCTION ru_trig();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function ru_trig", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER ru_tr BEFORE INSERT ON ru_t6 FOR EACH ROW EXECUTE FUNCTION ru_trig();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function ru_trig", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE RULE ru_r AS ON INSERT TO ru_t6 DO ALSO SELECT NULL::ru_en;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_en", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ru_d1 AS ru_ty;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_ty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ru_d2 AS INT CHECK (VALUE::ru_dom > 0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ru_d3 AS INT DEFAULT NULL::ru_dom;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ru_d4 AS ru_dom;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ru_d5 AS INT;",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN ru_d5 SET DEFAULT NULL::ru_dom;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN ru_d5 ADD CHECK (VALUE::ru_dom > 0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ru_rng AS RANGE (SUBTYPE = ru_dom);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ru_rng2 AS RANGE (SUBTYPE = FLOAT8, SUBTYPE_DIFF = ru_diff);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function ru_diff", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR === (LEFTARG = ru_ty, RIGHTARG = INT, FUNCTION = ru_a);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_ty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR === (LEFTARG = INT, RIGHTARG = INT, FUNCTION = ru_f);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function ru_f", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR === (LEFTARG = INT, RIGHTARG = INT, FUNCTION = int4pl, RESTRICT = eqsel);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function eqsel", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR === (LEFTARG = INT, RIGHTARG = INT, FUNCTION = int4pl, JOIN = eqjoinsel);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "only boolean operators can have join selectivity", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE ru_agg (INT) (SFUNC = ru_sfunc, STYPE = INT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function ru_sfunc", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE ru_agg2 (ru_dom) (SFUNC = int4pl, STYPE = INT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE ru_agg3 (INT) (SFUNC = int4pl, STYPE = ru_dom);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "return type of transition function int4pl is not ru_dom", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CAST (ru_ty AS INT) WITH INOUT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ru_ty or type integer", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL ru_proc();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for procedure ru_proc", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_like (LIKE ru_t4 INCLUDING ALL);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "ru_t4" does not exist"#, position: 28, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_src (a INT DEFAULT NULL::ru_dom, CHECK (a::ru_dom > 0));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON ru_src ((a::ru_dom));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_like1 (LIKE ru_src INCLUDING DEFAULTS);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table ru_src", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_like2 (LIKE ru_src INCLUDING CONSTRAINTS);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table ru_src", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_like3 (LIKE ru_src INCLUDING INDEXES);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table ru_src", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ru_like4 (LIKE ru_src);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table ru_src", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON LANGUAGE sql FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON LANGUAGE plpgsql FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_l1() RETURNS INT LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language sql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_l2() RETURNS INT LANGUAGE plpgsql AS 'BEGIN RETURN 1; END';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language plpgsql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_l3() RETURNS INT LANGUAGE internal AS 'int4pl';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language internal", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ru_l4() RETURNS INT LANGUAGE c AS 'x', 'y';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO 'BEGIN END';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language plpgsql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO LANGUAGE sql 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language sql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE sql TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE plpgsql TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO 'BEGIN END';",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('ru_seq');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for sequence ru_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT currval('ru_seq');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for sequence ru_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setval('ru_seq', 5);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for sequence ru_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_sequence_last_value('ru_seq');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_sequence_last_value", INT8)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ru_t (a) VALUES (1) RETURNING b AS ident;",
                    expected: Expected::Rows {
                        columns: &[Column("ident", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('ru_seq');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lastval();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for sequence ru_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT currval('ru_seq');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for sequence ru_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ru_seq TO ru_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT currval('ru_seq');",
                    expected: Expected::Rows {
                        columns: &[Column("currval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lastval();",
                    expected: Expected::Rows {
                        columns: &[Column("lastval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_sequence_last_value('ru_seq') IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('ru_seq');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for sequence ru_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON ru_seq TO ru_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('ru_seq');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setval('ru_seq', 10);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for sequence ru_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE ON ru_seq TO ru_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setval('ru_seq', 10);",
                    expected: Expected::Rows {
                        columns: &[Column("setval", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW config_file;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to examine "config_file""#, detail: r#"Only roles with privileges of the "pg_read_all_settings" role may examine this parameter."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('data_directory');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to examine "data_directory""#, detail: r#"Only roles with privileges of the "pg_read_all_settings" role may examine this parameter."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_settings WHERE name = 'config_file';",
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
                    query: "SELECT count(*) > 0 AS shown FROM pg_settings WHERE sourcefile IS NOT NULL AND sourcefile <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("shown", BOOL)],
                        rows: &[
                            &[T("f")],
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
                    query: "GRANT pg_read_all_settings TO ru_user;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ru_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('data_directory') <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_settings WHERE name = 'config_file';",
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
                    query: "SELECT count(*) > 0 AS shown FROM pg_settings WHERE sourcefile IS NOT NULL AND sourcefile <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("shown", BOOL)],
                        rows: &[
                            &[T("t")],
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
                    query: "SELECT count(*) > 0 AS shown FROM pg_settings WHERE sourcefile IS NOT NULL AND sourcefile <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("shown", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "GRANT and REVOKE on languages and ALTER LANGUAGE OWNER",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE rl_user LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE rl_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO rl_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON LANGUAGE sql FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanacl FROM pg_language WHERE lanname IN ('sql', 'plpgsql') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("plpgsql"), Null],
                            &[T("sql"), T("{postgres=U/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE rl_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION rl_f() RETURNS INT LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language sql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE sql TO rl_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE rl_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION rl_f() RETURNS INT LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON LANGUAGE plpgsql FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE rl_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO 'BEGIN END';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language plpgsql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION rl_g() RETURNS INT LANGUAGE plpgsql AS 'BEGIN RETURN 1; END';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language plpgsql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE plpgsql TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE sql TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON LANGUAGE sql FROM rl_user;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanacl FROM pg_language WHERE lanname IN ('sql', 'plpgsql') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("plpgsql"), T("{postgres=U/postgres,=U/postgres}")],
                            &[T("sql"), T("{postgres=U/postgres,=U/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE c TO rl_user;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"language "c" is not trusted"#, detail: "GRANT and REVOKE are not allowed on untrusted languages, because only superusers can use untrusted languages.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE internal TO rl_user;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"language "internal" is not trusted"#, detail: "GRANT and REVOKE are not allowed on untrusted languages, because only superusers can use untrusted languages.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON LANGUAGE plpgsql TO rl_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE missing TO rl_user;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"language "missing" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE rl_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE sql TO rl_owner;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01007", message: r#"no privileges were granted for "sql""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql OWNER TO rl_user;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of language plpgsql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql OWNER TO rl_owner;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanowner::regrole, lanacl FROM pg_language WHERE lanname = 'plpgsql';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanowner", REGROLE), Column("lanacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("plpgsql"), T("rl_owner"), T("{rl_owner=U/rl_owner,=U/rl_owner,rl_user=U/rl_owner}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql OWNER TO rl_owner;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE missing OWNER TO rl_owner;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"language "missing" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT rl_owner TO rl_user;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE rl_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql OWNER TO rl_user;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanowner::regrole, lanacl FROM pg_language WHERE lanname = 'plpgsql';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanowner", REGROLE), Column("lanacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("plpgsql"), T("rl_user"), T("{rl_user=U/rl_user,=U/rl_user}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql OWNER TO postgres;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname, lanowner::regrole, lanacl FROM pg_language WHERE lanname = 'plpgsql';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("lanowner", REGROLE), Column("lanacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("plpgsql"), T("postgres"), T("{postgres=U/postgres,=U/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE INDEX works as the table's owner with the safe search path",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE ri_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ri_t (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ri_t VALUES (1, 'x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ri_t OWNER TO ri_owner;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ri_f(INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$BEGIN RAISE NOTICE 'user %, path %', current_user, current_setting('search_path'); RETURN $1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ri_i ON ri_t ((ri_f(a)));",
                    expected: Expected::Tag("CREATE INDEX"),
                    notices: &[Diagnostic { code: "00000", message: "user ri_owner, path pg_catalog, pg_temp", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, current_setting('search_path');",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("postgres"), T(r#""$user", public"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA ri_s;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS ri_s.ri_ops FOR TYPE INT4 USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 btint4cmp(INT4, INT4);",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION ri_s.ri_coll (LOCALE = 'C', PROVIDER = builtin);",
                    expected: Expected::Tag("CREATE COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = ri_s, public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ri_o ON ri_t (a ri_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ri_c ON ri_t (b COLLATE ri_coll);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ri_m ON ri_t (a missing_ops);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"operator class "missing_ops" does not exist for access method "btree""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ri_n ON ri_t (b COLLATE missing_coll);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"collation "missing_coll" for encoding "UTF8" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET search_path;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass, indclass[0]::regclass::text <> '' AS has_class FROM pg_index WHERE indrelid = 'ri_t'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("has_class", BOOL)],
                        rows: &[
                            &[T("ri_i"), T("t")],
                            &[T("ri_o"), T("t")],
                            &[T("ri_c"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION ri_f(INT) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ri_j ON ri_t ((ri_f(a) + 1));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function ri_f", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION ri_f(INT) TO ri_owner;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ri_k ON ri_t ((ri_f(a) + 2));",
                    expected: Expected::Tag("CREATE INDEX"),
                    notices: &[Diagnostic { code: "00000", message: "user ri_owner, path pg_catalog, pg_temp", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Type USAGE for typed tables",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE rt_user LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE rt_ty AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE rt_open AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON TYPE rt_ty FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO rt_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE rt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rt_t1 OF rt_ty;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type rt_ty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rt_t2 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rt_t2 OF rt_ty;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for type rt_ty", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rt_t2 OF rt_open;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rt_t2 NOT OF;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON TYPE rt_ty TO rt_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE rt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rt_t1 OF rt_ty;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rt_t2 OF rt_ty;",
                    expected: Expected::Tag("ALTER TABLE"),
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
