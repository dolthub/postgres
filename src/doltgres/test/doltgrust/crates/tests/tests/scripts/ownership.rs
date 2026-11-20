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
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_ownership_checks() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER ... OWNER TO, and the owner, superuser, and CREATE checks of DDL",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE ob_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ob_other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO ob_owner;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA ob_s;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ob_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ob_f() RETURNS INT LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ob_t (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION ob_c FROM "C";"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ob_st ON a, b FROM ob_t;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ob_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE ob_t IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table ob_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN ob_t.a IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of relation ob_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON FUNCTION ob_f() IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function ob_f", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLLATION ob_c IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of collation ob_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON STATISTICS ob_st IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of statistics object ob_st", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON SCHEMA public IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON SCHEMA ob_s IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION ob_f();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function ob_f", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP COLLATION ob_c;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of collation ob_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS ob_st;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of statistics object ob_st", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION ob_f() RENAME TO ob_g;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function ob_f", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION ob_c RENAME TO ob_c2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of collation ob_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER STATISTICS ob_st RENAME TO ob_st2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of statistics object ob_st", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION ob_f() SET SCHEMA ob_s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION ob_c REFRESH VERSION;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of collation ob_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION ob_s.ob_c3 FROM "C";"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION ob_c4 FROM "C";"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ob_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION ob_f() RENAME TO ob_g;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION ob_g() SET SCHEMA ob_s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON FUNCTION ob_g() IS 'mine';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLLATION ob_c IS 'mine';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION ob_c REFRESH VERSION;",
                    expected: Expected::Tag("ALTER COLLATION"),
                    notices: &[Diagnostic { code: "00000", message: "version has not changed", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION ob_g();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ob_owner TO ob_other;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ob_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE ob_t IS 'member';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP COLLATION ob_c;",
                    expected: Expected::Tag("DROP COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE fn_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE fn_other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO fn_owner;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE fn_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ob_s.fn_f2() RETURNS INT LANGUAGE sql AS 'SELECT 2';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fn_f3() RETURNS INT LANGUAGE sql LEAKPROOF AS 'SELECT 3';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE fn_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fn_f4() RETURNS INT LANGUAGE sql LEAKPROOF AS 'SELECT 4';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "only superuser can define a leakproof function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fn_f5() RETURNS INT LANGUAGE sql AS 'SELECT 5';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION fn_f5() LEAKPROOF;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "only superuser can define a leakproof function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION fn_f5() SUPPORT generate_series_int4_support;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to specify a support function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fn_f6(INT) RETURNS INT LANGUAGE sql SUPPORT generate_series_int4_support AS 'SELECT 6';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to specify a support function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE fn_ty AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CAST (fn_ty AS INT4) WITHOUT FUNCTION;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create a cast WITHOUT FUNCTION", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CAST (fn_ty AS TEXT) WITH INOUT;",
                    expected: Expected::Tag("CREATE CAST"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE fn_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION fn_f5() STRICT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function fn_f5", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PROCEDURE fn_f5() STRICT;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "fn_f5() is not a procedure", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CAST (fn_ty AS INT8) WITH INOUT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type fn_ty or type bigint", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON CAST (fn_ty AS TEXT) IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type fn_ty or type text", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP CAST (fn_ty AS TEXT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type fn_ty or type text", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE op_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE op_other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO op_owner, op_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION op_sel(INTERNAL, OID, INTERNAL, INT4) RETURNS FLOAT8 LANGUAGE internal STABLE STRICT AS 'scalarltsel';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE op_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION op_fn(INT, INT) RETURNS BOOL LANGUAGE sql AS 'SELECT $1 < $2';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <<< (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR >>> (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn, COMMUTATOR = <<<);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR FAMILY op_fam USING btree;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create an operator family", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS op_cls FOR TYPE INT USING btree AS OPERATOR 1 <;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create an operator class", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR FAMILY integer_ops USING btree ADD OPERATOR 1 <<< (INT, INT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to alter an operator family", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <~~ (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn, RESTRICT = scalarltsel);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <~~~ (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn, RESTRICT = op_sel);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to specify a non-built-in restriction estimator function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR ~<~ (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn, COMMUTATOR = ~>~);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE op_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR <<< (INT, INT) SET (RESTRICT = scalarltsel);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of operator <<<", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR ~~> (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn, COMMUTATOR = <<<);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of operator <<<", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR ~~> (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn, NEGATOR = <<<);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of operator <<<", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR ~>~ (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of operator ~>~", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR ~~> (LEFTARG = INT, RIGHTARG = INT, FUNCTION = op_fn, COMMUTATOR = OPERATOR(ob_s.~<<));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON OPERATOR <<< (INT, INT) IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of operator <<<", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR <<< (INT, INT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of operator <<<", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE op_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR <<< (INT, INT) SET (RESTRICT = scalarltsel);",
                    expected: Expected::Tag("ALTER OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR <<< (INT, INT) SET (COMMUTATOR = >>>);",
                    expected: Expected::Tag("ALTER OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sc_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sc_other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO sc_owner, sc_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sc_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sc_t (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sc_f() RETURNS INT LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE sc_shell;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create a base type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS sc_st ON a, b FROM sc_t;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE RULE sc_r AS ON INSERT TO sc_t DO ALSO NOTHING;",
                    expected: Expected::Tag("CREATE RULE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY sc_p ON sc_t USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sc_s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for database postgres", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sc_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION sc_f() RETURNS INT LANGUAGE sql AS 'SELECT 2';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function sc_f", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE sc_shell AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS sc_st2 ON a, b FROM sc_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table sc_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER STATISTICS sc_st SET STATISTICS 10;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of statistics object sc_st", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE RULE sc_r2 AS ON UPDATE TO sc_t DO ALSO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table sc_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sc_t DISABLE RULE sc_r;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table sc_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER RULE sc_r ON sc_t RENAME TO sc_r3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table sc_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY sc_p2 ON sc_t USING (true);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table sc_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY sc_p ON sc_t USING (false);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table sc_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY sc_p ON sc_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of relation sc_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA sc_s RENAME TO sc_s2;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: r#"schema "sc_s" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sc_s3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for database postgres", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sc_s4 AUTHORIZATION sc_owner;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for database postgres", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON DATABASE postgres TO sc_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sc_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sc_s7;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sc_s8 AUTHORIZATION sc_owner;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"must be able to SET ROLE "sc_owner""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA sc_s7 RENAME TO sc_s9;",
                    expected: Expected::Tag("ALTER SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sc_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA sc_s RENAME TO sc_s6;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: r#"schema "sc_s" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CREATE ON DATABASE postgres FROM sc_other;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ty_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ty_other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO ty_owner, ty_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ty_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ty_e AS ENUM ('a');",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ty_d AS INT;",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ty_r AS RANGE (SUBTYPE = INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ty_c AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ty_base;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create a base type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ty_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ob_s.ty_d2 AS INT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ob_s.ty_e2 AS ENUM ('a');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ob_s.ty_r3 AS RANGE (SUBTYPE = INT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ty_r2 AS RANGE (SUBTYPE = INT, MULTIRANGE_TYPE_NAME = ob_s.ty_mr);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_e ADD VALUE 'b';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_e", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_e RENAME VALUE 'a' TO 'c';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_e", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_e RENAME TO ty_e3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_e", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_e SET SCHEMA ob_s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema ob_s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_e SET SCHEMA public;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_e", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN ty_d SET DEFAULT 1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_d", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN ty_d ADD CONSTRAINT ty_ck CHECK (VALUE > 0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_d", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN ty_d RENAME TO ty_d3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_d", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_c SET (STORAGE = main);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_c SET (RECEIVE = NONE);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to alter a type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_c RENAME ATTRIBUTE a TO b;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table ty_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TYPE ty_c IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TYPE ty_c[] IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON DOMAIN ty_d IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_d", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TYPE ty_c;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TYPE ty_c[];",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type ty_c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ty_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_e RENAME TO ty_e4;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ty_c SET (RECEIVE = NONE);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to alter a type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE tb_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE tb_other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO tb_owner, tb_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA tb_sch AUTHORIZATION tb_owner;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE tb_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb_t (a INT PRIMARY KEY, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb_p (a INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE tb_seq;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb_s (a SERIAL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW tb_v AS SELECT 1 AS x;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb_sch.tb_in (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE tb_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t ADD COLUMN c INT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t RENAME TO tb_t2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t RENAME COLUMN b TO c;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t RENAME CONSTRAINT tb_t_pkey TO tb_k;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t SET SCHEMA ob_s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER VIEW tb_v RENAME TO tb_v2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of view tb_v", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SEQUENCE tb_seq RESTART;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of sequence tb_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t OWNER TO tb_other;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE tb_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW tb_v;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of view tb_v", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SEQUENCE tb_seq;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of sequence tb_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb_c () INHERITS (tb_t);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb_f (a INT REFERENCES tb_p);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table tb_p", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE tb_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN tb_t.b IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of relation tb_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT REFERENCES (a) ON tb_p TO tb_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT TRUNCATE ON tb_s TO tb_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE tb_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb_f (a INT REFERENCES tb_p);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE tb_s RESTART IDENTITY;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of sequence tb_s_a_seq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE tb_s;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE tb_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE tb_sch.tb_in;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb_sch.tb_in2 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE tb_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE tb_sch.tb_in2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t RENAME TO tb_t3;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CREATE ON SCHEMA public FROM tb_owner;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE tb_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t3 RENAME TO tb_t4;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tb_t3 ADD COLUMN d INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ix_owner;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ix_other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO ix_owner;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ix_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ix_t (a INT PRIMARY KEY, b INT[]);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ix_i ON ix_t (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ix_g ON ix_t USING gin (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ix_r (a INT REFERENCES ix_t);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ix_tf() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER ix_tr BEFORE INSERT ON ix_t FOR EACH ROW EXECUTE FUNCTION ix_tf();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CREATE ON SCHEMA public FROM ix_owner;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ix_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ix_i2 ON ix_t (b);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ix_t ADD UNIQUE (a);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO ix_owner;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ix_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ix_i3 ON ix_t (a);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table ix_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX ix_i;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for index ix_i", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE ix_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table ix_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX SCHEMA public;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX DATABASE postgres;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of database postgres", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE ix_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table ix_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER ix_tr2 BEFORE INSERT ON ix_t FOR EACH ROW EXECUTE FUNCTION ix_tf();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table ix_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ix_t DISABLE TRIGGER ix_tr;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table ix_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TRIGGER ix_tr ON ix_t RENAME TO ix_tr3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table ix_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER ix_tr ON ix_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of relation ix_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM ix_t;",
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"permission denied to vacuum "ix_t", skipping it"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ix_t;",
                    expected: Expected::Tag("ANALYZE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"permission denied to analyze "ix_t", skipping it"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (ANALYZE) ix_t;",
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"permission denied to vacuum "ix_t", skipping it"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT gin_clean_pending_list('ix_g');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of index ix_g", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX ix_i;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of index ix_i", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX ix_i RENAME TO ix_i4;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of index ix_i", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT MAINTAIN ON ix_t TO ix_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT TRIGGER ON ix_t TO ix_other;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ix_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX ix_i;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE ix_t;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM ix_t;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ix_t;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX (TABLESPACE pg_global) TABLE ix_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for tablespace pg_global", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER ix_tr2 BEFORE INSERT ON ix_t FOR EACH ROW EXECUTE FUNCTION ix_tf();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ix_owner;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ix_t DISABLE TRIGGER ALL;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied: "RI_ConstraintTrigger_a_16504" is a system trigger"#, ..E }),
                    skip: Some("the message names a system trigger by its OID, which Postgres and Doltgrust hand out differently"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ix_r DISABLE TRIGGER ALL;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied: "RI_ConstraintTrigger_c_16506" is a system trigger"#, ..E }),
                    skip: Some("the message names a system trigger by its OID, which Postgres and Doltgrust hand out differently"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ow_a;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ow_b;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ow_c;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO ow_a;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ow_b TO ow_a;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ow_a;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ow_t (a SERIAL PRIMARY KEY, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ow_i ON ow_t (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW ow_v AS SELECT 1 AS x;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ow_f() RETURNS INT LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ow_ty AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE ow_e AS ENUM ('x');",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN ow_d AS INT;",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION ow_co FROM "C";"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ow_st ON a, b FROM ow_t;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA ow_s AUTHORIZATION ow_a;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ow_a;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ow_t OWNER TO ow_b;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO ow_b;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ow_a;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ow_t OWNER TO ow_b;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ow_t OWNER TO ow_c;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"must be able to SET ROLE "ow_c""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX ow_i OWNER TO ow_a;",
                    expected: Expected::Tag("ALTER INDEX"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42809", message: r#"cannot change owner of index "ow_i""#, hint: "Change the ownership of the index's table instead.", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SEQUENCE ow_t_a_seq OWNER TO ow_a;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot change owner of sequence "ow_t_a_seq""#, detail: r#"Sequence "ow_t_a_seq" is linked to table "ow_t"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER VIEW ow_v OWNER TO ow_b;",
                    expected: Expected::Tag("ALTER VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION ow_f() OWNER TO ow_b;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ow_ty OWNER TO ow_b;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ow_ty[] OWNER TO ow_b;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "[""#, position: 17, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ow_t OWNER TO ow_b;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "ow_t is a table's row type", hint: "Use ALTER TABLE instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE ow_e OWNER TO ow_b;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN ow_d OWNER TO ow_b;",
                    expected: Expected::Tag("ALTER DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN ow_e OWNER TO ow_b;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "ow_e is not a domain", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION ow_co OWNER TO ow_b;",
                    expected: Expected::Tag("ALTER COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER STATISTICS ow_st OWNER TO ow_b;",
                    expected: Expected::Tag("ALTER STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA ow_s OWNER TO ow_b;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for database postgres", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ow_ty OWNER TO ow_b;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""ow_ty" is a composite type"#, hint: "Use ALTER TYPE instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT c.relname, r.rolname FROM pg_class c JOIN pg_roles r ON r.oid = c.relowner WHERE c.relname LIKE 'ow\_%' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("rolname", NAME)],
                        rows: &[
                            &[T("ow_i"), T("ow_b")],
                            &[T("ow_t"), T("ow_b")],
                            &[T("ow_t_a_seq"), T("ow_b")],
                            &[T("ow_t_pkey"), T("ow_b")],
                            &[T("ow_ty"), T("ow_b")],
                            &[T("ow_v"), T("ow_b")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT t.typname, r.rolname FROM pg_type t JOIN pg_roles r ON r.oid = t.typowner WHERE t.typname LIKE 'ow\_%' OR t.typname LIKE '\_ow\_%' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("rolname", NAME)],
                        rows: &[
                            &[T("_ow_d"), T("ow_b")],
                            &[T("_ow_e"), T("ow_b")],
                            &[T("_ow_t"), T("ow_b")],
                            &[T("_ow_ty"), T("ow_b")],
                            &[T("_ow_v"), T("ow_b")],
                            &[T("ow_d"), T("ow_b")],
                            &[T("ow_e"), T("ow_b")],
                            &[T("ow_t"), T("ow_b")],
                            &[T("ow_ty"), T("ow_b")],
                            &[T("ow_v"), T("ow_b")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n.nspname, r.rolname FROM pg_namespace n JOIN pg_roles r ON r.oid = n.nspowner WHERE n.nspname = 'ow_s';",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME), Column("rolname", NAME)],
                        rows: &[
                            &[T("ow_s"), T("ow_a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ow_b;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION ow_f() OWNER TO ow_c;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"must be able to SET ROLE "ow_c""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ow_t OWNER TO ow_a;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"must be able to SET ROLE "ow_a""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ow_t OWNER TO ow_c;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION ow_f() OWNER TO ow_c;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ow_t TO ow_a;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT (b) ON ow_t TO ow_b;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ow_t OWNER TO ow_a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE relname = 'ow_t';",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{ow_a=arwdDxtm/ow_a}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attacl FROM pg_attribute WHERE attrelid = 'ow_t'::regclass AND attname = 'b';",
                    expected: Expected::Rows {
                        columns: &[Column("attacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{ow_b=r/ow_a}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ow_etf() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER ow_et ON ddl_command_start EXECUTE FUNCTION ow_etf();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER ow_et OWNER TO ow_a;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to change owner of event trigger "ow_et""#, hint: "The owner of an event trigger must be a superuser.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER ow_et OWNER TO ow_c;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to change owner of event trigger "ow_et""#, hint: "The owner of an event trigger must be a superuser.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER ow_et OWNER TO postgres;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER ow_et;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
