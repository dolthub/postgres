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
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};

#[test]
fn test_row_security() {
    run_scripts(&[
        ScriptTest {
            name: "PG18's rowsecurity regression test",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET client_min_messages TO 'warning';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER IF EXISTS regress_rls_alice;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER IF EXISTS regress_rls_bob;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER IF EXISTS regress_rls_carol;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER IF EXISTS regress_rls_dave;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER IF EXISTS regress_rls_exempt_user;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE IF EXISTS regress_rls_group1;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE IF EXISTS regress_rls_group2;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA IF EXISTS regress_rls_schema CASCADE;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET client_min_messages;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER regress_rls_alice NOLOGIN;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER regress_rls_bob NOLOGIN;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER regress_rls_carol NOLOGIN;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER regress_rls_dave NOLOGIN;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER regress_rls_exempt_user BYPASSRLS NOLOGIN;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_rls_group1 NOLOGIN;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_rls_group2 NOLOGIN;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT regress_rls_group1 TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT regress_rls_group2 TO regress_rls_carol;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA regress_rls_schema;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON SCHEMA regress_rls_schema to public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = regress_rls_schema;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION f_leak(text) RETURNS bool COST 0.0000001 LANGUAGE plpgsql AS 'BEGIN RAISE NOTICE ''f_leak => %'', $1; RETURN true; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION f_leak(text) TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE uaccount ( pguser      name primary key, seclv       int );",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON uaccount TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO uaccount VALUES ('regress_rls_alice', 99), ('regress_rls_bob', 1), ('regress_rls_carol', 2), ('regress_rls_dave', 3);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE category ( cid        int primary key, cname      text );",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON category TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO category VALUES (11, 'novel'), (22, 'science fiction'), (33, 'technology'), (44, 'manga');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE document ( did         int primary key, cid         int references category(cid), dlevel      int not null, dauthor     name, dtitle      text );",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON document TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES ( 1, 11, 1, 'regress_rls_bob', 'my first novel'), ( 2, 11, 2, 'regress_rls_bob', 'my second novel'), ( 3, 22, 2, 'regress_rls_bob', 'my science fiction'), ( 4, 44, 1, 'regress_rls_bob', 'my first manga'), ( 5, 44, 2, 'regress_rls_bob', 'my second manga'), ( 6, 22, 1, 'regress_rls_carol', 'great science fiction'), ( 7, 33, 2, 'regress_rls_carol', 'great technology book'), ( 8, 44, 1, 'regress_rls_carol', 'great manga'), ( 9, 22, 1, 'regress_rls_dave', 'awesome science fiction'), (10, 33, 2, 'regress_rls_dave', 'awesome technology book');",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE document ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON document AS PERMISSIVE USING (dlevel <= (SELECT seclv FROM uaccount WHERE pguser = current_user));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON document AS UGLY USING (dlevel <= (SELECT seclv FROM uaccount WHERE pguser = current_user));",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized row security option "ugly""#, hint: "Only PERMISSIVE or RESTRICTIVE policies are supported currently.", position: 33, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2r ON document AS RESTRICTIVE TO regress_rls_dave USING (cid <> 44 AND cid < 50);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1r ON document AS RESTRICTIVE TO regress_rls_dave USING (cid <> 44);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_policies WHERE schemaname = 'regress_rls_schema' AND tablename = 'document' ORDER BY policyname;",
                    expected: Expected::Rows {
                        columns: &[Column("schemaname", NAME), Column("tablename", NAME), Column("policyname", NAME), Column("permissive", TEXT), Column("roles", NAME_ARRAY), Column("cmd", TEXT), Column("qual", TEXT), Column("with_check", TEXT)],
                        rows: &[
                            &[T("regress_rls_schema"), T("document"), T("p1"), T("PERMISSIVE"), T("{public}"), T("ALL"), T("(dlevel <= ( SELECT uaccount.seclv\n   FROM uaccount\n  WHERE (uaccount.pguser = CURRENT_USER)))"), Null],
                            &[T("regress_rls_schema"), T("document"), T("p1r"), T("RESTRICTIVE"), T("{regress_rls_dave}"), T("ALL"), T("(cid <> 44)"), Null],
                            &[T("regress_rls_schema"), T("document"), T("p2r"), T("RESTRICTIVE"), T("{regress_rls_dave}"), T("ALL"), T("((cid <> 44) AND (cid < 50))"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                        ],
                        tag: "SELECT 5",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my first manga", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great manga", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document NATURAL JOIN category WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("did", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("1"), T("1"), T("regress_rls_bob"), T("my first novel"), T("novel")],
                            &[T("44"), T("4"), T("1"), T("regress_rls_bob"), T("my first manga"), T("manga")],
                            &[T("22"), T("6"), T("1"), T("regress_rls_carol"), T("great science fiction"), T("science fiction")],
                            &[T("44"), T("8"), T("1"), T("regress_rls_carol"), T("great manga"), T("manga")],
                            &[T("22"), T("9"), T("1"), T("regress_rls_dave"), T("awesome science fiction"), T("science fiction")],
                        ],
                        tag: "SELECT 5",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my first manga", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great manga", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                        ],
                        tag: "SELECT 10",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => my first manga", ..N }, Diagnostic { code: "00000", message: "f_leak => my second manga", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }, Diagnostic { code: "00000", message: "f_leak => great manga", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome technology book", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document NATURAL JOIN category WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("did", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("1"), T("1"), T("regress_rls_bob"), T("my first novel"), T("novel")],
                            &[T("11"), T("2"), T("2"), T("regress_rls_bob"), T("my second novel"), T("novel")],
                            &[T("22"), T("3"), T("2"), T("regress_rls_bob"), T("my science fiction"), T("science fiction")],
                            &[T("44"), T("4"), T("1"), T("regress_rls_bob"), T("my first manga"), T("manga")],
                            &[T("44"), T("5"), T("2"), T("regress_rls_bob"), T("my second manga"), T("manga")],
                            &[T("22"), T("6"), T("1"), T("regress_rls_carol"), T("great science fiction"), T("science fiction")],
                            &[T("33"), T("7"), T("2"), T("regress_rls_carol"), T("great technology book"), T("technology")],
                            &[T("44"), T("8"), T("1"), T("regress_rls_carol"), T("great manga"), T("manga")],
                            &[T("22"), T("9"), T("1"), T("regress_rls_dave"), T("awesome science fiction"), T("science fiction")],
                            &[T("33"), T("10"), T("2"), T("regress_rls_dave"), T("awesome technology book"), T("technology")],
                        ],
                        tag: "SELECT 10",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => my first manga", ..N }, Diagnostic { code: "00000", message: "f_leak => my second manga", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }, Diagnostic { code: "00000", message: "f_leak => great manga", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome technology book", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on document"#)],
                            &[T(r#"  Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM document NATURAL JOIN category WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Hash Join"#)],
                            &[T(r#"  Hash Cond: (category.cid = document.cid)"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                            &[T(r#"  ->  Seq Scan on category"#)],
                            &[T(r#"  ->  Hash"#)],
                            &[T(r#"        ->  Seq Scan on document"#)],
                            &[T(r#"              Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_dave;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                        ],
                        tag: "SELECT 7",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome technology book", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document NATURAL JOIN category WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("did", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("1"), T("1"), T("regress_rls_bob"), T("my first novel"), T("novel")],
                            &[T("11"), T("2"), T("2"), T("regress_rls_bob"), T("my second novel"), T("novel")],
                            &[T("22"), T("3"), T("2"), T("regress_rls_bob"), T("my science fiction"), T("science fiction")],
                            &[T("22"), T("6"), T("1"), T("regress_rls_carol"), T("great science fiction"), T("science fiction")],
                            &[T("33"), T("7"), T("2"), T("regress_rls_carol"), T("great technology book"), T("technology")],
                            &[T("22"), T("9"), T("1"), T("regress_rls_dave"), T("awesome science fiction"), T("science fiction")],
                            &[T("33"), T("10"), T("2"), T("regress_rls_dave"), T("awesome technology book"), T("technology")],
                        ],
                        tag: "SELECT 7",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome technology book", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on document"#)],
                            &[T(r#"  Filter: ((cid <> 44) AND (cid <> 44) AND (cid < 50) AND (dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM document NATURAL JOIN category WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Hash Join"#)],
                            &[T(r#"  Hash Cond: (category.cid = document.cid)"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                            &[T(r#"  ->  Seq Scan on category"#)],
                            &[T(r#"  ->  Hash"#)],
                            &[T(r#"        ->  Seq Scan on document"#)],
                            &[T(r#"              Filter: ((cid <> 44) AND (cid <> 44) AND (cid < 50) AND (dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (100, 44, 1, 'regress_rls_dave', 'testing sorting of policies');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy "p1r" for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (100, 55, 1, 'regress_rls_dave', 'testing sorting of policies');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy "p2r" for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY p1 ON document USING (true);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table document", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON document;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of relation document", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY p1 ON document USING (dauthor = current_user);",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga")],
                        ],
                        tag: "SELECT 5",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => my first manga", ..N }, Diagnostic { code: "00000", message: "f_leak => my second manga", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document NATURAL JOIN category WHERE f_leak(dtitle) ORDER by did;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("did", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("1"), T("1"), T("regress_rls_bob"), T("my first novel"), T("novel")],
                            &[T("11"), T("2"), T("2"), T("regress_rls_bob"), T("my second novel"), T("novel")],
                            &[T("22"), T("3"), T("2"), T("regress_rls_bob"), T("my science fiction"), T("science fiction")],
                            &[T("44"), T("4"), T("1"), T("regress_rls_bob"), T("my first manga"), T("manga")],
                            &[T("44"), T("5"), T("2"), T("regress_rls_bob"), T("my second manga"), T("manga")],
                        ],
                        tag: "SELECT 5",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => my first manga", ..N }, Diagnostic { code: "00000", message: "f_leak => my second manga", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga")],
                        ],
                        tag: "SELECT 3",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }, Diagnostic { code: "00000", message: "f_leak => great manga", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document NATURAL JOIN category WHERE f_leak(dtitle) ORDER by did;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("did", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("cname", TEXT)],
                        rows: &[
                            &[T("22"), T("6"), T("1"), T("regress_rls_carol"), T("great science fiction"), T("science fiction")],
                            &[T("33"), T("7"), T("2"), T("regress_rls_carol"), T("great technology book"), T("technology")],
                            &[T("44"), T("8"), T("1"), T("regress_rls_carol"), T("great manga"), T("manga")],
                        ],
                        tag: "SELECT 3",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }, Diagnostic { code: "00000", message: "f_leak => great manga", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on document"#)],
                            &[T(r#"  Filter: ((dauthor = CURRENT_USER) AND f_leak(dtitle))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM document NATURAL JOIN category WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  ->  Seq Scan on document"#)],
                            &[T(r#"        Filter: ((dauthor = CURRENT_USER) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Index Scan using category_pkey on category"#)],
                            &[T(r#"        Index Cond: (cid = document.cid)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON category USING (CASE WHEN current_user = 'regress_rls_bob' THEN cid IN (11, 33) WHEN current_user = 'regress_rls_carol' THEN cid IN (22, 44) ELSE false END);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE category ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document d FULL OUTER JOIN category c on d.cid = c.cid ORDER BY d.did, c.cid;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("cid", INT4), Column("cname", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel"), T("11"), T("novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel"), T("11"), T("novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction"), Null, Null],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga"), Null, Null],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga"), Null, Null],
                            &[Null, Null, Null, Null, Null, T("33"), T("technology")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM category WHERE cid = 33;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"update or delete on table "category" violates foreign key constraint "document_cid_fkey" on table "document""#, detail: r#"Key is still referenced from table "document"."#, schema: "regress_rls_schema", table: "document", constraint: "document_cid_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document d FULL OUTER JOIN category c on d.cid = c.cid ORDER BY d.did, c.cid;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("cid", INT4), Column("cname", TEXT)],
                        rows: &[
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction"), T("22"), T("science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book"), Null, Null],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga"), T("44"), T("manga")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (11, 33, 1, current_user, 'hoge');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (8, 44, 1, 'regress_rls_bob', 'my third manga');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "document_pkey""#, schema: "regress_rls_schema", table: "document", constraint: "document_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE did = 8;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (8, 44, 1, 'regress_rls_carol', 'my third manga');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE document SET did = 8, dauthor = 'regress_rls_carol' WHERE did = 5;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("11"), T("33"), T("1"), T("regress_rls_carol"), T("hoge")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM category;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("novel")],
                            &[T("22"), T("science fiction")],
                            &[T("33"), T("technology")],
                            &[T("44"), T("manga")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("11"), T("33"), T("1"), T("regress_rls_carol"), T("hoge")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM category;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("novel")],
                            &[T("22"), T("science fiction")],
                            &[T("33"), T("technology")],
                            &[T("44"), T("manga")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("11"), T("33"), T("1"), T("regress_rls_carol"), T("hoge")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM category;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("novel")],
                            &[T("22"), T("science fiction")],
                            &[T("33"), T("technology")],
                            &[T("44"), T("manga")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("11"), T("33"), T("1"), T("regress_rls_carol"), T("hoge")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM category;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("novel")],
                            &[T("22"), T("science fiction")],
                            &[T("33"), T("technology")],
                            &[T("44"), T("manga")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("11"), T("33"), T("1"), T("regress_rls_carol"), T("hoge")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM category;",
                    expected: Expected::Rows {
                        columns: &[Column("cid", INT4), Column("cname", TEXT)],
                        rows: &[
                            &[T("11"), T("novel")],
                            &[T("22"), T("science fiction")],
                            &[T("33"), T("technology")],
                            &[T("44"), T("manga")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (id int not null primary key, a int, junk1 text, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 DROP COLUMN junk1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON t1 TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (c float) INHERITS (t1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON t2 TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (id int not null primary key, c text, b text, a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t3 INHERIT t1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON t3 TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON t1 FOR ALL TO PUBLIC USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON t2 FOR ALL TO PUBLIC USING (a % 2 = 1);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT *, t1 FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT *, t1 FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("t1", USER_DEFINED)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT *, t1 FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 FOR SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1 FOR SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"              Filter: ((a % 2) = 0)"#)],
                            &[T(r#"        ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"              Filter: ((a % 2) = 0)"#)],
                            &[T(r#"        ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"              Filter: ((a % 2) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 WHERE f_leak(b) FOR SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1 WHERE f_leak(b) FOR SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"              Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"        ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"              Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"        ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"              Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, tableoid::regclass FROM t2 UNION ALL SELECT a, b, tableoid::regclass FROM t3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("tableoid", REGCLASS)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b, tableoid::regclass FROM t2 UNION ALL SELECT a, b, tableoid::regclass FROM t3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t2"#)],
                            &[T(r#"        Filter: ((a % 2) = 1)"#)],
                            &[T(r#"  ->  Seq Scan on t3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE part_document ( did         int, cid         int, dlevel      int not null, dauthor     name, dtitle      text ) PARTITION BY RANGE (cid);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON part_document TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE part_document_fiction PARTITION OF part_document FOR VALUES FROM (11) to (12);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE part_document_satire PARTITION OF part_document FOR VALUES FROM (55) to (56);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE part_document_nonfiction PARTITION OF part_document FOR VALUES FROM (99) to (100);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON part_document_fiction TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON part_document_satire TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON part_document_nonfiction TO public;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO part_document VALUES ( 1, 11, 1, 'regress_rls_bob', 'my first novel'), ( 2, 11, 2, 'regress_rls_bob', 'my second novel'), ( 3, 99, 2, 'regress_rls_bob', 'my science textbook'), ( 4, 55, 1, 'regress_rls_bob', 'my first satire'), ( 5, 99, 2, 'regress_rls_bob', 'my history book'), ( 6, 11, 1, 'regress_rls_carol', 'great science fiction'), ( 7, 99, 2, 'regress_rls_carol', 'great technology book'), ( 8, 55, 2, 'regress_rls_carol', 'great satire'), ( 9, 11, 1, 'regress_rls_dave', 'awesome science fiction'), (10, 99, 2, 'regress_rls_dave', 'awesome technology book');",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE part_document ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY pp1 ON part_document AS PERMISSIVE USING (dlevel <= (SELECT seclv FROM uaccount WHERE pguser = current_user));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY pp1r ON part_document AS RESTRICTIVE TO regress_rls_dave USING (cid < 55);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_policies WHERE schemaname = 'regress_rls_schema' AND tablename like '%part_document%' ORDER BY policyname;",
                    expected: Expected::Rows {
                        columns: &[Column("schemaname", NAME), Column("tablename", NAME), Column("policyname", NAME), Column("permissive", TEXT), Column("roles", NAME_ARRAY), Column("cmd", TEXT), Column("qual", TEXT), Column("with_check", TEXT)],
                        rows: &[
                            &[T("regress_rls_schema"), T("part_document"), T("pp1"), T("PERMISSIVE"), T("{public}"), T("ALL"), T("(dlevel <= ( SELECT uaccount.seclv\n   FROM uaccount\n  WHERE (uaccount.pguser = CURRENT_USER)))"), Null],
                            &[T("regress_rls_schema"), T("part_document"), T("pp1r"), T("RESTRICTIVE"), T("{regress_rls_dave}"), T("ALL"), T("(cid < 55)"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                        ],
                        tag: "SELECT 4",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => my first satire", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM part_document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                            &[T(r#"  ->  Seq Scan on part_document_fiction part_document_1"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Seq Scan on part_document_satire part_document_2"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Seq Scan on part_document_nonfiction part_document_3"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("99"), T("2"), T("regress_rls_bob"), T("my science textbook")],
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("5"), T("99"), T("2"), T("regress_rls_bob"), T("my history book")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("99"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("99"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                        ],
                        tag: "SELECT 10",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => my first satire", ..N }, Diagnostic { code: "00000", message: "f_leak => great satire", ..N }, Diagnostic { code: "00000", message: "f_leak => my science textbook", ..N }, Diagnostic { code: "00000", message: "f_leak => my history book", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome technology book", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM part_document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                            &[T(r#"  ->  Seq Scan on part_document_fiction part_document_1"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Seq Scan on part_document_satire part_document_2"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Seq Scan on part_document_nonfiction part_document_3"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_dave;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                        ],
                        tag: "SELECT 4",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM part_document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on part_document_fiction part_document"#)],
                            &[T(r#"  Filter: ((cid < 55) AND (dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO part_document VALUES (100, 11, 5, 'regress_rls_dave', 'testing pp1');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "part_document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO part_document VALUES (100, 99, 1, 'regress_rls_dave', 'testing pp1r');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy "pp1r" for table "part_document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO part_document VALUES (100, 55, 1, 'regress_rls_dave', 'testing RLS with partitions');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy "pp1r" for table "part_document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO part_document_satire VALUES (100, 55, 1, 'regress_rls_dave', 'testing RLS with partitions');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                        ],
                        tag: "SELECT 4",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document_satire WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("100"), T("55"), T("1"), T("regress_rls_dave"), T("testing RLS with partitions")],
                        ],
                        tag: "SELECT 3",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first satire", ..N }, Diagnostic { code: "00000", message: "f_leak => great satire", ..N }, Diagnostic { code: "00000", message: "f_leak => testing RLS with partitions", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE part_document_satire ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY pp3 ON part_document_satire AS RESTRICTIVE USING (cid < 55);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_dave;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO part_document_satire VALUES (101, 55, 1, 'regress_rls_dave', 'testing RLS with partitions');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "part_document_satire""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document_satire WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                        ],
                        tag: "SELECT 4",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM part_document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on part_document_fiction part_document"#)],
                            &[T(r#"  Filter: ((cid < 55) AND (dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("99"), T("2"), T("regress_rls_bob"), T("my science textbook")],
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("5"), T("99"), T("2"), T("regress_rls_bob"), T("my history book")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("99"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("99"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("100"), T("55"), T("1"), T("regress_rls_dave"), T("testing RLS with partitions")],
                        ],
                        tag: "SELECT 11",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => my first satire", ..N }, Diagnostic { code: "00000", message: "f_leak => great satire", ..N }, Diagnostic { code: "00000", message: "f_leak => testing RLS with partitions", ..N }, Diagnostic { code: "00000", message: "f_leak => my science textbook", ..N }, Diagnostic { code: "00000", message: "f_leak => my history book", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }, Diagnostic { code: "00000", message: "f_leak => awesome technology book", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM part_document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Index Scan using uaccount_pkey on uaccount"#)],
                            &[T(r#"          Index Cond: (pguser = CURRENT_USER)"#)],
                            &[T(r#"  ->  Seq Scan on part_document_fiction part_document_1"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Seq Scan on part_document_satire part_document_2"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Seq Scan on part_document_nonfiction part_document_3"#)],
                            &[T(r#"        Filter: ((dlevel <= (InitPlan 1).col1) AND f_leak(dtitle))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY pp1 ON part_document USING (true);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table part_document", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY pp1 ON part_document;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of relation part_document", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY pp1 ON part_document USING (dauthor = current_user);",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("99"), T("2"), T("regress_rls_bob"), T("my science textbook")],
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("5"), T("99"), T("2"), T("regress_rls_bob"), T("my history book")],
                        ],
                        tag: "SELECT 5",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => my first novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my second novel", ..N }, Diagnostic { code: "00000", message: "f_leak => my first satire", ..N }, Diagnostic { code: "00000", message: "f_leak => my science textbook", ..N }, Diagnostic { code: "00000", message: "f_leak => my history book", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document WHERE f_leak(dtitle) ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("99"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                        ],
                        tag: "SELECT 3",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => great science fiction", ..N }, Diagnostic { code: "00000", message: "f_leak => great satire", ..N }, Diagnostic { code: "00000", message: "f_leak => great technology book", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM part_document WHERE f_leak(dtitle);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on part_document_fiction part_document_1"#)],
                            &[T(r#"        Filter: ((dauthor = CURRENT_USER) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Seq Scan on part_document_satire part_document_2"#)],
                            &[T(r#"        Filter: ((dauthor = CURRENT_USER) AND f_leak(dtitle))"#)],
                            &[T(r#"  ->  Seq Scan on part_document_nonfiction part_document_3"#)],
                            &[T(r#"        Filter: ((dauthor = CURRENT_USER) AND f_leak(dtitle))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("99"), T("2"), T("regress_rls_bob"), T("my science textbook")],
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("5"), T("99"), T("2"), T("regress_rls_bob"), T("my history book")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("99"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("99"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("100"), T("55"), T("1"), T("regress_rls_dave"), T("testing RLS with partitions")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document_satire ORDER by did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("100"), T("55"), T("1"), T("regress_rls_dave"), T("testing RLS with partitions")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document ORDER BY did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("99"), T("2"), T("regress_rls_bob"), T("my science textbook")],
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("5"), T("99"), T("2"), T("regress_rls_bob"), T("my history book")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("99"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("99"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("100"), T("55"), T("1"), T("regress_rls_dave"), T("testing RLS with partitions")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document_satire ORDER by did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("100"), T("55"), T("1"), T("regress_rls_dave"), T("testing RLS with partitions")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document ORDER by did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                            &[T("3"), T("99"), T("2"), T("regress_rls_bob"), T("my science textbook")],
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("5"), T("99"), T("2"), T("regress_rls_bob"), T("my history book")],
                            &[T("6"), T("11"), T("1"), T("regress_rls_carol"), T("great science fiction")],
                            &[T("7"), T("99"), T("2"), T("regress_rls_carol"), T("great technology book")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("9"), T("11"), T("1"), T("regress_rls_dave"), T("awesome science fiction")],
                            &[T("10"), T("99"), T("2"), T("regress_rls_dave"), T("awesome technology book")],
                            &[T("100"), T("55"), T("1"), T("regress_rls_dave"), T("testing RLS with partitions")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document_satire ORDER by did;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("4"), T("55"), T("1"), T("regress_rls_bob"), T("my first satire")],
                            &[T("8"), T("55"), T("2"), T("regress_rls_carol"), T("great satire")],
                            &[T("100"), T("55"), T("1"), T("regress_rls_dave"), T("testing RLS with partitions")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_dave;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document ORDER by did;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "part_document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM part_document_satire ORDER by did;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "part_document_satire""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY pp3 ON part_document AS RESTRICTIVE USING ((SELECT dlevel <= seclv FROM uaccount WHERE pguser = current_user));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO part_document VALUES (100, 11, 5, 'regress_rls_carol', 'testing pp3');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy "pp3" for table "part_document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dependee (x integer, y integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dependent (x integer, y integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY d1 ON dependent FOR ALL TO PUBLIC USING (x = (SELECT d.x FROM dependee d WHERE d.y = y));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE dependee;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table dependee because other objects depend on it", detail: "policy d1 on table dependent depends on table dependee", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE dependee CASCADE;",
                    expected: Expected::Tag("DROP TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to policy d1 on table dependent", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM dependent;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on dependent"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rec1 (x integer, y integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY r1 ON rec1 USING (x = (SELECT r.x FROM rec1 r WHERE y = r.y));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rec1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rec1;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in policy for relation "rec1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rec2 (a integer, b integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY r1 ON rec1 USING (x = (SELECT a FROM rec2 WHERE b = y));",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY r2 ON rec2 USING (a = (SELECT x FROM rec1 WHERE y = b));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rec2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rec1;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in policy for relation "rec1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rec1v AS SELECT * FROM rec1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rec2v AS SELECT * FROM rec2;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY r1 ON rec1 USING (x = (SELECT a FROM rec2v WHERE b = y));",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY r2 ON rec2 USING (a = (SELECT x FROM rec1v WHERE y = b));",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rec1;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in policy for relation "rec1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rec1v, rec2v CASCADE;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 2 other objects", detail: "drop cascades to policy r1 on table rec1\ndrop cascades to policy r2 on table rec2", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rec1v WITH (security_barrier) AS SELECT * FROM rec1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rec2v WITH (security_barrier) AS SELECT * FROM rec2;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY r1 ON rec1 USING (x = (SELECT a FROM rec2v WHERE b = y));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY r2 ON rec2 USING (a = (SELECT x FROM rec1v WHERE y = b));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rec1;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in policy for relation "rec1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE s1 (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s1 (SELECT x, public.fipshash(x::text) FROM generate_series(-10,10) x);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.fipshash(text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE s2 (x int, y text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s2 (SELECT x, public.fipshash(x::text) FROM generate_series(-6,6) x);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.fipshash(text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON s1, s2 TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON s1 USING (a in (select x from s2 where y like '%2f%'));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON s2 USING (x in (select a from s1 where b like '%22%'));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON s1 FOR INSERT WITH CHECK (a = (SELECT a FROM s1));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE s1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE s2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v2 AS SELECT * FROM s2 WHERE y like '%af%';",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s1 WHERE f_leak(b);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in policy for relation "s1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s1 VALUES (1, 'foo');",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in policy for relation "s1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p3 on s1;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY p2 ON s2 USING (x % 2 = 0);",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM only s1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on s1"#)],
                            &[T(r#"  Filter: ((ANY (a = (hashed SubPlan 1).col1)) AND f_leak(b))"#)],
                            &[T(r#"  SubPlan 1"#)],
                            &[T(r#"    ->  Seq Scan on s2"#)],
                            &[T(r#"          Filter: (((x % 2) = 0) AND (y ~~ '%2f%'::text))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY p1 ON s1 USING (a in (select x from v2));",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on s1"#)],
                            &[T(r#"  Filter: ((ANY (a = (hashed SubPlan 1).col1)) AND f_leak(b))"#)],
                            &[T(r#"  SubPlan 1"#)],
                            &[T(r#"    ->  Seq Scan on s2"#)],
                            &[T(r#"          Filter: (((x % 2) = 0) AND (y ~~ '%af%'::text))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT x FROM s1 LIMIT 1) xx, * FROM s2 WHERE y like '%28%';",
                    expected: Expected::Rows {
                        columns: &[Column("xx", INT4), Column("x", INT4), Column("y", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT (SELECT x FROM s1 LIMIT 1) xx, * FROM s2 WHERE y like '%28%';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on s2"#)],
                            &[T(r#"  Filter: (((x % 2) = 0) AND (y ~~ '%28%'::text))"#)],
                            &[T(r#"  SubPlan 2"#)],
                            &[T(r#"    ->  Limit"#)],
                            &[T(r#"          ->  Seq Scan on s1"#)],
                            &[T(r#"                Filter: (ANY (a = (hashed SubPlan 1).col1))"#)],
                            &[T(r#"                SubPlan 1"#)],
                            &[T(r#"                  ->  Seq Scan on s2 s2_1"#)],
                            &[T(r#"                        Filter: (((x % 2) = 0) AND (y ~~ '%af%'::text))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY p2 ON s2 USING (x in (select a from s1 where b like '%d2%'));",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s1 WHERE f_leak(b);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"infinite recursion detected in policy for relation "s1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p1(int) AS SELECT * FROM t1 WHERE a <= $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p1(2);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p1(2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: ((a <= 2) AND ((a % 2) = 0))"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: ((a <= 2) AND ((a % 2) = 0))"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: ((a <= 2) AND ((a % 2) = 0))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: f_leak(b)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p1(2);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p1(2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: (a <= 2)"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: (a <= 2)"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: (a <= 2)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p2(int) AS SELECT * FROM t1 WHERE a = $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p2(2);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p2(2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: (a = 2)"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: (a = 2)"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: (a = 2)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p2(2);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p2(2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"        Filter: ((a = 2) AND ((a % 2) = 0))"#)],
                            &[T(r#"  ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"        Filter: ((a = 2) AND ((a % 2) = 0))"#)],
                            &[T(r#"  ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"        Filter: ((a = 2) AND ((a % 2) = 0))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE t1 SET b = b || b WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on t1"#)],
                            &[T(r#"  Update on t1 t1_1"#)],
                            &[T(r#"  Update on t2 t1_2"#)],
                            &[T(r#"  Update on t3 t1_3"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"                    Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"              ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"                    Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"              ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"                    Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 SET b = b || b WHERE f_leak(b);",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE only t1 SET b = b || '_updt' WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on t1"#)],
                            &[T(r#"  ->  Seq Scan on t1"#)],
                            &[T(r#"        Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE only t1 SET b = b || '_updt' WHERE f_leak(b);",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE only t1 SET b = b WHERE f_leak(b) RETURNING tableoid::regclass, *, t1;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("t1", USER_DEFINED)],
                        rows: &[
                        ],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 SET b = b WHERE f_leak(b) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 SET b = b WHERE f_leak(b) RETURNING tableoid::regclass, *, t1;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("t1", USER_DEFINED)],
                        rows: &[
                        ],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE t2 SET b=t2.b FROM t3 WHERE t2.a = 3 and t3.a = 2 AND f_leak(t2.b) AND f_leak(t3.b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on t2"#)],
                            &[T(r#"  ->  Nested Loop"#)],
                            &[T(r#"        ->  Seq Scan on t2"#)],
                            &[T(r#"              Filter: ((a = 3) AND ((a % 2) = 1) AND f_leak(b))"#)],
                            &[T(r#"        ->  Seq Scan on t3"#)],
                            &[T(r#"              Filter: ((a = 2) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t2 SET b=t2.b FROM t3 WHERE t2.a = 3 and t3.a = 2 AND f_leak(t2.b) AND f_leak(t3.b);",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE t1 SET b=t1.b FROM t2 WHERE t1.a = 3 and t2.a = 3 AND f_leak(t1.b) AND f_leak(t2.b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on t1"#)],
                            &[T(r#"  Update on t1 t1_1"#)],
                            &[T(r#"  Update on t2 t1_2"#)],
                            &[T(r#"  Update on t3 t1_3"#)],
                            &[T(r#"  ->  Nested Loop"#)],
                            &[T(r#"        ->  Seq Scan on t2"#)],
                            &[T(r#"              Filter: ((a = 3) AND ((a % 2) = 1) AND f_leak(b))"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"                    Filter: ((a = 3) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"              ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"                    Filter: ((a = 3) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"              ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"                    Filter: ((a = 3) AND ((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 SET b=t1.b FROM t2 WHERE t1.a = 3 and t2.a = 3 AND f_leak(t1.b) AND f_leak(t2.b);",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE t2 SET b=t2.b FROM t1 WHERE t1.a = 3 and t2.a = 3 AND f_leak(t1.b) AND f_leak(t2.b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on t2"#)],
                            &[T(r#"  ->  Nested Loop"#)],
                            &[T(r#"        ->  Seq Scan on t2"#)],
                            &[T(r#"              Filter: ((a = 3) AND ((a % 2) = 1) AND f_leak(b))"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"                    Filter: ((a = 3) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"              ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"                    Filter: ((a = 3) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"              ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"                    Filter: ((a = 3) AND ((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t2 SET b=t2.b FROM t1 WHERE t1.a = 3 and t2.a = 3 AND f_leak(t1.b) AND f_leak(t2.b);",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE t2 t2_1 SET b = t2_2.b FROM t2 t2_2 WHERE t2_1.a = 3 AND t2_2.a = t2_1.a AND t2_2.b = t2_1.b AND f_leak(t2_1.b) AND f_leak(t2_2.b) RETURNING *, t2_1, t2_2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on t2 t2_1"#)],
                            &[T(r#"  ->  Nested Loop"#)],
                            &[T(r#"        Join Filter: (t2_1.b = t2_2.b)"#)],
                            &[T(r#"        ->  Seq Scan on t2 t2_1"#)],
                            &[T(r#"              Filter: ((a = 3) AND ((a % 2) = 1) AND f_leak(b))"#)],
                            &[T(r#"        ->  Seq Scan on t2 t2_2"#)],
                            &[T(r#"              Filter: ((a = 3) AND ((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t2 t2_1 SET b = t2_2.b FROM t2 t2_2 WHERE t2_1.a = 3 AND t2_2.a = t2_1.a AND t2_2.b = t2_1.b AND f_leak(t2_1.b) AND f_leak(t2_2.b) RETURNING *, t2_1, t2_2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("c", FLOAT8), Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("c", FLOAT8), Column("t2_1", USER_DEFINED), Column("t2_2", USER_DEFINED)],
                        rows: &[
                        ],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE t1 t1_1 SET b = t1_2.b FROM t1 t1_2 WHERE t1_1.a = 4 AND t1_2.a = t1_1.a AND t1_2.b = t1_1.b AND f_leak(t1_1.b) AND f_leak(t1_2.b) RETURNING *, t1_1, t1_2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on t1 t1_1"#)],
                            &[T(r#"  Update on t1 t1_1_1"#)],
                            &[T(r#"  Update on t2 t1_1_2"#)],
                            &[T(r#"  Update on t3 t1_1_3"#)],
                            &[T(r#"  ->  Nested Loop"#)],
                            &[T(r#"        Join Filter: (t1_1.b = t1_2.b)"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on t1 t1_1_1"#)],
                            &[T(r#"                    Filter: ((a = 4) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"              ->  Seq Scan on t2 t1_1_2"#)],
                            &[T(r#"                    Filter: ((a = 4) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"              ->  Seq Scan on t3 t1_1_3"#)],
                            &[T(r#"                    Filter: ((a = 4) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"        ->  Materialize"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Seq Scan on t1 t1_2_1"#)],
                            &[T(r#"                          Filter: ((a = 4) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"                    ->  Seq Scan on t2 t1_2_2"#)],
                            &[T(r#"                          Filter: ((a = 4) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"                    ->  Seq Scan on t3 t1_2_3"#)],
                            &[T(r#"                          Filter: ((a = 4) AND ((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 t1_1 SET b = t1_2.b FROM t1 t1_2 WHERE t1_1.a = 4 AND t1_2.a = t1_1.a AND t1_2.b = t1_1.b AND f_leak(t1_1.b) AND f_leak(t1_2.b) RETURNING *, t1_1, t1_2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("t1_1", USER_DEFINED), Column("t1_2", USER_DEFINED)],
                        rows: &[
                        ],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a,b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM only t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Delete on t1"#)],
                            &[T(r#"  ->  Seq Scan on t1"#)],
                            &[T(r#"        Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM t1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Delete on t1"#)],
                            &[T(r#"  Delete on t1 t1_1"#)],
                            &[T(r#"  Delete on t2 t1_2"#)],
                            &[T(r#"  Delete on t3 t1_3"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on t1 t1_1"#)],
                            &[T(r#"              Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"        ->  Seq Scan on t2 t1_2"#)],
                            &[T(r#"              Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"        ->  Seq Scan on t3 t1_3"#)],
                            &[T(r#"              Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM only t1 WHERE f_leak(b) RETURNING tableoid::regclass, *, t1;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("t1", USER_DEFINED)],
                        rows: &[
                        ],
                        tag: "DELETE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t1 WHERE f_leak(b) RETURNING tableoid::regclass, *, t1;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("t1", USER_DEFINED)],
                        rows: &[
                        ],
                        tag: "DELETE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b1 (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b1 (SELECT x, public.fipshash(x::text) FROM generate_series(-10,10) x);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.fipshash(text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON b1 USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE b1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON b1 TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW bv1 WITH (security_barrier) AS SELECT * FROM b1 WHERE a > 0 WITH CHECK OPTION;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON bv1 TO regress_rls_carol;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bv1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Subquery Scan on bv1"#)],
                            &[T(r#"  Filter: f_leak(bv1.b)"#)],
                            &[T(r#"  ->  Seq Scan on b1"#)],
                            &[T(r#"        Filter: ((a > 0) AND ((a % 2) = 0))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM bv1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bv1 VALUES (-1, 'xxx');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "b1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bv1 VALUES (11, 'xxx');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "b1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bv1 VALUES (12, 'xxx');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE bv1 SET b = 'yyy' WHERE a = 4 AND f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on b1"#)],
                            &[T(r#"  ->  Seq Scan on b1"#)],
                            &[T(r#"        Filter: ((a > 0) AND (a = 4) AND ((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE bv1 SET b = 'yyy' WHERE a = 4 AND f_leak(b);",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM bv1 WHERE a = 6 AND f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Delete on b1"#)],
                            &[T(r#"  ->  Seq Scan on b1"#)],
                            &[T(r#"        Filter: ((a > 0) AND (a = 6) AND ((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM bv1 WHERE a = 6 AND f_leak(b);",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM b1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("12"), T("xxx")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1r ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON document FOR SELECT USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON document FOR INSERT WITH CHECK (dauthor = current_user);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON document FOR UPDATE USING (cid = (SELECT cid from category WHERE cname = 'novel')) WITH CHECK (dauthor = current_user);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE did = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my second novel")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (2, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_carol', 'my first novel') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle, dauthor = EXCLUDED.dauthor;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (33, 22, 1, 'regress_rls_bob', 'okay science fiction');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (33, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_bob', 'Some novel, replaces sci-fi') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy (USING expression) for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (2, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_bob', 'my first novel') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my first novel")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (78, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_bob', 'some technology novel') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle, cid = 33 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("78"), T("11"), T("1"), T("regress_rls_bob"), T("some technology novel")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (78, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_bob', 'some technology novel') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle, cid = 33 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("78"), T("33"), T("1"), T("regress_rls_bob"), T("some technology novel")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (78, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_bob', 'some technology novel') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle, cid = 33 RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy (USING expression) for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (79, (SELECT cid from category WHERE cname = 'technology'), 1, 'regress_rls_bob', 'technology book, can only insert') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT)],
                        rows: &[
                            &[T("79"), T("33"), T("1"), T("regress_rls_bob"), T("technology book, can only insert")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (79, (SELECT cid from category WHERE cname = 'technology'), 1, 'regress_rls_bob', 'technology book, can only insert') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy (USING expression) for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p2 ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p3 ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3_with_default ON document FOR UPDATE USING (cid = (SELECT cid from category WHERE cname = 'novel'));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (79, (SELECT cid from category WHERE cname = 'technology'), 1, 'regress_rls_bob', 'technology book, can only insert') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (2, (SELECT cid from category WHERE cname = 'technology'), 1, 'regress_rls_bob', 'my first novel') ON CONFLICT (did) DO UPDATE SET cid = EXCLUDED.cid, dtitle = EXCLUDED.dtitle RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p3_with_default ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3_with_all ON document FOR ALL USING (cid = (SELECT cid from category WHERE cname = 'novel')) WITH CHECK (dauthor = current_user);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (80, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_carol', 'my first novel') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle, cid = 33;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (4, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_bob', 'my first novel') ON CONFLICT (did) DO UPDATE SET dtitle = EXCLUDED.dtitle;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy (USING expression) for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO document VALUES (1, (SELECT cid from category WHERE cname = 'novel'), 1, 'regress_rls_bob', 'my first novel') ON CONFLICT (did) DO UPDATE SET dauthor = 'regress_rls_carol';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p3_with_all ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE document ADD COLUMN dnotes text DEFAULT '';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON document FOR SELECT USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON document FOR INSERT WITH CHECK (dauthor = current_user);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON document FOR UPDATE USING (cid = (SELECT cid from category WHERE cname = 'novel')) WITH CHECK (dlevel > 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p4 ON document FOR DELETE USING (cid = (SELECT cid from category WHERE cname = 'manga'));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("dnotes", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel"), T("")],
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction"), T("")],
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga"), T("")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga"), T("")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction"), T("")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book"), T("")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga"), T("")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction"), T("")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book"), T("")],
                            &[T("11"), T("33"), T("1"), T("regress_rls_carol"), T("hoge"), T("")],
                            &[T("33"), T("22"), T("1"), T("regress_rls_bob"), T("okay science fiction"), T("")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my first novel"), T("")],
                            &[T("78"), T("33"), T("1"), T("regress_rls_bob"), T("some technology novel"), T("")],
                            &[T("79"), T("33"), T("1"), T("regress_rls_bob"), T("technology book, can only insert"), T("")],
                        ],
                        tag: "SELECT 14",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 1 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge1 ', dlevel = 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 1 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge2 ';",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 1 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge3 ', dlevel = 1;",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 3 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge ';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"target row violates row-level security policy (USING expression) for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 3 as sdid) s ON did = s.sdid WHEN MATCHED THEN DELETE;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"target row violates row-level security policy (USING expression) for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 4 as sdid) s ON did = s.sdid WHEN MATCHED AND dnotes = '' THEN UPDATE SET dnotes = dnotes || ' notes added by merge ' WHEN MATCHED THEN DELETE;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"target row violates row-level security policy (USING expression) for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 4 as sdid) s ON did = s.sdid WHEN MATCHED AND dnotes <> '' THEN UPDATE SET dnotes = dnotes || ' notes added by merge ' WHEN MATCHED THEN DELETE;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"target row violates row-level security policy (USING expression) for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 4 as sdid) s ON did = s.sdid WHEN MATCHED AND dnotes <> '' THEN UPDATE SET dnotes = dnotes || ' notes added by merge ' WHEN MATCHED THEN DO NOTHING;",
                    expected: Expected::Tag("MERGE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE did = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("dnotes", TEXT)],
                        rows: &[
                            &[T("4"), T("44"), T("1"), T("regress_rls_bob"), T("my first manga"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 4 as sdid) s ON did = s.sdid WHEN MATCHED AND dnotes <> '' THEN UPDATE SET dnotes = dnotes || ' notes added by merge ' WHEN MATCHED THEN DELETE;",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 12 as sdid) s ON did = s.sdid WHEN MATCHED THEN DELETE WHEN NOT MATCHED THEN INSERT VALUES (12, 11, 1, 'regress_rls_dave', 'another novel');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 12 as sdid) s ON did = s.sdid WHEN MATCHED THEN DELETE WHEN NOT MATCHED THEN INSERT VALUES (12, 11, 1, 'regress_rls_bob', 'another novel');",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 1 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge4 ' WHEN NOT MATCHED THEN INSERT VALUES (12, 11, 1, 'regress_rls_bob', 'another novel');",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON document FOR SELECT USING (cid = (SELECT cid from category WHERE cname = 'novel'));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 7 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge5 ' WHEN NOT MATCHED THEN INSERT VALUES (12, 11, 1, 'regress_rls_bob', 'another novel');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "document_pkey""#, schema: "regress_rls_schema", table: "document", constraint: "document_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 1 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge6 ', cid = (SELECT cid from category WHERE cname = 'technology');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 1 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge7 ', cid = (SELECT cid from category WHERE cname = 'novel');",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 13 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge8 ' WHEN NOT MATCHED THEN INSERT VALUES (13, 44, 1, 'regress_rls_bob', 'new manga');",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document WHERE did = 13;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("dnotes", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 14 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge9 ' WHEN NOT MATCHED THEN INSERT VALUES (14, 44, 1, 'regress_rls_bob', 'new manga') RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "document""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO document d USING (SELECT 14 as sdid) s ON did = s.sdid WHEN MATCHED THEN UPDATE SET dnotes = dnotes || ' notes added by merge10 ' WHEN NOT MATCHED THEN INSERT VALUES (14, 11, 1, 'regress_rls_bob', 'new novel') RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("sdid", INT4), Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("dnotes", TEXT)],
                        rows: &[
                            &[T("14"), T("14"), T("11"), T("1"), T("regress_rls_bob"), T("new novel"), T("")],
                        ],
                        tag: "MERGE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON document;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM document;",
                    expected: Expected::Rows {
                        columns: &[Column("did", INT4), Column("cid", INT4), Column("dlevel", INT4), Column("dauthor", NAME), Column("dtitle", TEXT), Column("dnotes", TEXT)],
                        rows: &[
                            &[T("3"), T("22"), T("2"), T("regress_rls_bob"), T("my science fiction"), T("")],
                            &[T("5"), T("44"), T("2"), T("regress_rls_bob"), T("my second manga"), T("")],
                            &[T("6"), T("22"), T("1"), T("regress_rls_carol"), T("great science fiction"), T("")],
                            &[T("7"), T("33"), T("2"), T("regress_rls_carol"), T("great technology book"), T("")],
                            &[T("8"), T("44"), T("1"), T("regress_rls_carol"), T("great manga"), T("")],
                            &[T("9"), T("22"), T("1"), T("regress_rls_dave"), T("awesome science fiction"), T("")],
                            &[T("10"), T("33"), T("2"), T("regress_rls_dave"), T("awesome technology book"), T("")],
                            &[T("11"), T("33"), T("1"), T("regress_rls_carol"), T("hoge"), T("")],
                            &[T("33"), T("22"), T("1"), T("regress_rls_bob"), T("okay science fiction"), T("")],
                            &[T("2"), T("11"), T("2"), T("regress_rls_bob"), T("my first novel"), T("")],
                            &[T("78"), T("33"), T("1"), T("regress_rls_bob"), T("some technology novel"), T("")],
                            &[T("79"), T("33"), T("1"), T("regress_rls_bob"), T("technology book, can only insert"), T("")],
                            &[T("12"), T("11"), T("1"), T("regress_rls_bob"), T("another novel"), T("")],
                            &[T("1"), T("11"), T("1"), T("regress_rls_bob"), T("my first novel"), T(" notes added by merge2  notes added by merge3  notes added by merge4  notes added by merge7 ")],
                            &[T("13"), T("44"), T("1"), T("regress_rls_bob"), T("new manga"), T("")],
                            &[T("14"), T("11"), T("1"), T("regress_rls_bob"), T("new novel"), T("")],
                        ],
                        tag: "SELECT 16",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE z1 (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE z2 (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON z1,z2 TO regress_rls_group1, regress_rls_group2, regress_rls_bob, regress_rls_carol;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO z1 VALUES (1, 'aba'), (2, 'bbb'), (3, 'ccc'), (4, 'dad');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON z1 TO regress_rls_group1 USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON z1 TO regress_rls_group2 USING (a % 2 = 1);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE z1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE plancache_test AS SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE plancache_test2 AS WITH q AS MATERIALIZED (SELECT * FROM z1 WHERE f_leak(b)) SELECT * FROM q,z2;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  CTE q"#)],
                            &[T(r#"    ->  Seq Scan on z1"#)],
                            &[T(r#"          Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"  ->  CTE Scan on q"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on z2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE plancache_test3 AS WITH q AS MATERIALIZED (SELECT * FROM z2) SELECT * FROM q,z1 WHERE f_leak(z1.b);",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  CTE q"#)],
                            &[T(r#"    ->  Seq Scan on z2"#)],
                            &[T(r#"  ->  CTE Scan on q"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on z1"#)],
                            &[T(r#"              Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_group1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  CTE q"#)],
                            &[T(r#"    ->  Seq Scan on z1"#)],
                            &[T(r#"          Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"  ->  CTE Scan on q"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on z2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  CTE q"#)],
                            &[T(r#"    ->  Seq Scan on z2"#)],
                            &[T(r#"  ->  CTE Scan on q"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on z1"#)],
                            &[T(r#"              Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                            &[T("3"), T("ccc")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }, Diagnostic { code: "00000", message: "f_leak => ccc", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  CTE q"#)],
                            &[T(r#"    ->  Seq Scan on z1"#)],
                            &[T(r#"          Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                            &[T(r#"  ->  CTE Scan on q"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on z2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  CTE q"#)],
                            &[T(r#"    ->  Seq Scan on z2"#)],
                            &[T(r#"  ->  CTE Scan on q"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on z1"#)],
                            &[T(r#"              Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_group2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                            &[T("3"), T("ccc")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }, Diagnostic { code: "00000", message: "f_leak => ccc", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  CTE q"#)],
                            &[T(r#"    ->  Seq Scan on z1"#)],
                            &[T(r#"          Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                            &[T(r#"  ->  CTE Scan on q"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on z2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE plancache_test3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  CTE q"#)],
                            &[T(r#"    ->  Seq Scan on z2"#)],
                            &[T(r#"  ->  CTE Scan on q"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on z1"#)],
                            &[T(r#"              Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rls_view AS SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_view TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                            &[T("2"), T("bbb")],
                            &[T("3"), T("ccc")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 4",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }, Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => ccc", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: f_leak(b)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                            &[T("2"), T("bbb")],
                            &[T("3"), T("ccc")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 4",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }, Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => ccc", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: f_leak(b)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rls_view AS SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_view TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for view rls_view", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for view rls_view", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_view TO regress_rls_carol;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE z1_blacklist (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO z1_blacklist VALUES (3), (4);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON z1 AS RESTRICTIVE USING (a NOT IN (SELECT a FROM z1_blacklist));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON z1_blacklist TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: ((NOT (ANY (a = (hashed SubPlan 1).col1))) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"  SubPlan 1"#)],
                            &[T(r#"    ->  Seq Scan on z1_blacklist"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: ((NOT (ANY (a = (hashed SubPlan 1).col1))) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"  SubPlan 1"#)],
                            &[T(r#"    ->  Seq Scan on z1_blacklist"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON z1_blacklist FROM regress_rls_bob;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p3 ON z1;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rls_view WITH (security_invoker) AS SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_view TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_view TO regress_rls_carol;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                            &[T("2"), T("bbb")],
                            &[T("3"), T("ccc")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 4",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }, Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => ccc", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: f_leak(b)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                            &[T("3"), T("ccc")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }, Diagnostic { code: "00000", message: "f_leak => ccc", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rls_view WITH (security_invoker) AS SELECT * FROM z1 WHERE f_leak(b);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_view TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_view TO regress_rls_carol;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                            &[T("2"), T("bbb")],
                            &[T("3"), T("ccc")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 4",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }, Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => ccc", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: f_leak(b)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                            &[T("4"), T("dad")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }, Diagnostic { code: "00000", message: "f_leak => dad", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                            &[T("3"), T("ccc")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }, Diagnostic { code: "00000", message: "f_leak => ccc", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: (((a % 2) = 1) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON z1 AS RESTRICTIVE USING (a NOT IN (SELECT a FROM z1_blacklist));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON z1_blacklist TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("bbb")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => bbb", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: ((NOT (ANY (a = (hashed SubPlan 1).col1))) AND ((a % 2) = 0) AND f_leak(b))"#)],
                            &[T(r#"  SubPlan 1"#)],
                            &[T(r#"    ->  Seq Scan on z1_blacklist"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table z1_blacklist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON z1_blacklist TO regress_rls_carol;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aba")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => aba", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on z1"#)],
                            &[T(r#"  Filter: ((NOT (ANY (a = (hashed SubPlan 1).col1))) AND ((a % 2) = 1) AND f_leak(b))"#)],
                            &[T(r#"  SubPlan 1"#)],
                            &[T(r#"    ->  Seq Scan on z1_blacklist"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE x1 (a int, b text, c text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON x1 TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x1 VALUES (1, 'abc', 'regress_rls_bob'), (2, 'bcd', 'regress_rls_bob'), (3, 'cde', 'regress_rls_carol'), (4, 'def', 'regress_rls_carol'), (5, 'efg', 'regress_rls_bob'), (6, 'fgh', 'regress_rls_bob'), (7, 'fgh', 'regress_rls_carol'), (8, 'fgh', 'regress_rls_carol');",
                    expected: Expected::Tag("INSERT 0 8"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p0 ON x1 FOR ALL USING (c = current_user);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON x1 FOR SELECT USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON x1 FOR INSERT WITH CHECK (a % 2 = 1);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON x1 FOR UPDATE USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p4 ON x1 FOR DELETE USING (a < 8);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE x1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x1 WHERE f_leak(b) ORDER BY a ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("abc"), T("regress_rls_bob")],
                            &[T("2"), T("bcd"), T("regress_rls_bob")],
                            &[T("4"), T("def"), T("regress_rls_carol")],
                            &[T("5"), T("efg"), T("regress_rls_bob")],
                            &[T("6"), T("fgh"), T("regress_rls_bob")],
                            &[T("8"), T("fgh"), T("regress_rls_carol")],
                        ],
                        tag: "SELECT 6",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => abc", ..N }, Diagnostic { code: "00000", message: "f_leak => bcd", ..N }, Diagnostic { code: "00000", message: "f_leak => def", ..N }, Diagnostic { code: "00000", message: "f_leak => efg", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE x1 SET b = b || '_updt' WHERE f_leak(b) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("abc_updt"), T("regress_rls_bob")],
                            &[T("2"), T("bcd_updt"), T("regress_rls_bob")],
                            &[T("4"), T("def_updt"), T("regress_rls_carol")],
                            &[T("5"), T("efg_updt"), T("regress_rls_bob")],
                            &[T("6"), T("fgh_updt"), T("regress_rls_bob")],
                            &[T("8"), T("fgh_updt"), T("regress_rls_carol")],
                        ],
                        tag: "UPDATE 6",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => abc", ..N }, Diagnostic { code: "00000", message: "f_leak => bcd", ..N }, Diagnostic { code: "00000", message: "f_leak => def", ..N }, Diagnostic { code: "00000", message: "f_leak => efg", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x1 WHERE f_leak(b) ORDER BY a ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("2"), T("bcd_updt"), T("regress_rls_bob")],
                            &[T("3"), T("cde"), T("regress_rls_carol")],
                            &[T("4"), T("def_updt"), T("regress_rls_carol")],
                            &[T("6"), T("fgh_updt"), T("regress_rls_bob")],
                            &[T("7"), T("fgh"), T("regress_rls_carol")],
                            &[T("8"), T("fgh_updt"), T("regress_rls_carol")],
                        ],
                        tag: "SELECT 6",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => cde", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh", ..N }, Diagnostic { code: "00000", message: "f_leak => bcd_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => def_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh_updt", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE x1 SET b = b || '_updt' WHERE f_leak(b) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("3"), T("cde_updt"), T("regress_rls_carol")],
                            &[T("7"), T("fgh_updt"), T("regress_rls_carol")],
                            &[T("2"), T("bcd_updt_updt"), T("regress_rls_bob")],
                            &[T("4"), T("def_updt_updt"), T("regress_rls_carol")],
                            &[T("6"), T("fgh_updt_updt"), T("regress_rls_bob")],
                            &[T("8"), T("fgh_updt_updt"), T("regress_rls_carol")],
                        ],
                        tag: "UPDATE 6",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => cde", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh", ..N }, Diagnostic { code: "00000", message: "f_leak => bcd_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => def_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh_updt", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM x1 WHERE f_leak(b) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("3"), T("cde_updt"), T("regress_rls_carol")],
                            &[T("7"), T("fgh_updt"), T("regress_rls_carol")],
                            &[T("2"), T("bcd_updt_updt"), T("regress_rls_bob")],
                            &[T("4"), T("def_updt_updt"), T("regress_rls_carol")],
                            &[T("6"), T("fgh_updt_updt"), T("regress_rls_bob")],
                            &[T("8"), T("fgh_updt_updt"), T("regress_rls_carol")],
                        ],
                        tag: "DELETE 6",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => cde_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => bcd_updt_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => def_updt_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh_updt_updt", ..N }, Diagnostic { code: "00000", message: "f_leak => fgh_updt_updt", ..N }],
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY), so f_leak's notices come in another order"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE y1 (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE y2 (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON y1, y2 TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON y1 FOR ALL USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON y1 FOR SELECT USING (a > 2);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON y1 FOR SELECT USING (a % 2 = 1);",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"policy "p1" for table "y1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON y2 FOR ALL USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE y1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE y2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rls_sbv WITH (security_barrier) AS SELECT * FROM y1 WHERE f_leak(b);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_sbv WHERE (a = 1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on y1"#)],
                            &[T(r#"  Filter: (f_leak(b) AND (a = 1))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_sbv;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rls_sbv WITH (security_barrier) AS SELECT * FROM y1 WHERE f_leak(b);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_sbv WHERE (a = 1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on y1"#)],
                            &[T(r#"  Filter: ((a = 1) AND ((a > 2) OR ((a % 2) = 0)) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_sbv;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO y2 (SELECT x, public.fipshash(x::text) FROM generate_series(0,20) x);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.fipshash(text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON y2 USING (a % 3 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON y2 USING (a % 4 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM y2 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM y2 WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on y2"#)],
                            &[T(r#"  Filter: ((((a % 4) = 0) OR ((a % 3) = 0) OR ((a % 2) = 0)) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM y2 WHERE f_leak('abc');",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM y2 WHERE f_leak('abc');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on y2"#)],
                            &[T(r#"  Filter: (f_leak('abc'::text) AND (((a % 4) = 0) OR ((a % 3) = 0) OR ((a % 2) = 0)))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test_qual_pushdown ( abc text );",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test_qual_pushdown VALUES ('abc'),('def');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM y2 JOIN test_qual_pushdown ON (b = abc) WHERE f_leak(abc);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("abc", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    notices: &[Diagnostic { code: "00000", message: "f_leak => abc", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM y2 JOIN test_qual_pushdown ON (b = abc) WHERE f_leak(abc);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Hash Join"#)],
                            &[T(r#"  Hash Cond: (test_qual_pushdown.abc = y2.b)"#)],
                            &[T(r#"  ->  Seq Scan on test_qual_pushdown"#)],
                            &[T(r#"        Filter: f_leak(abc)"#)],
                            &[T(r#"  ->  Hash"#)],
                            &[T(r#"        ->  Seq Scan on y2"#)],
                            &[T(r#"              Filter: (((a % 4) = 0) OR ((a % 3) = 0) OR ((a % 2) = 0))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM y2 JOIN test_qual_pushdown ON (b = abc) WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("abc", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM y2 JOIN test_qual_pushdown ON (b = abc) WHERE f_leak(b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Hash Join"#)],
                            &[T(r#"  Hash Cond: (test_qual_pushdown.abc = y2.b)"#)],
                            &[T(r#"  ->  Seq Scan on test_qual_pushdown"#)],
                            &[T(r#"  ->  Hash"#)],
                            &[T(r#"        ->  Seq Scan on y2"#)],
                            &[T(r#"              Filter: ((((a % 4) = 0) OR ((a % 3) = 0) OR ((a % 2) = 0)) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test_qual_pushdown;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t1 CASCADE;",
                    expected: Expected::Tag("DROP TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 2 other objects", detail: "drop cascades to table t2\ndrop cascades to table t3", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (a integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON t1 TO regress_rls_bob, regress_rls_carol;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON t1 TO regress_rls_bob USING ((a % 2) = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON t1 TO regress_rls_carol USING ((a % 4) = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE role_inval AS SELECT * FROM t1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE role_inval;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on t1"#)],
                            &[T(r#"  Filter: ((a % 2) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE role_inval;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on t1"#)],
                            &[T(r#"  Filter: ((a % 4) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE role_inval;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on t1"#)],
                            &[T(r#"  Filter: ((a % 2) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t1 CASCADE;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (a integer, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON t1 USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON t1 TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 (SELECT x, public.fipshash(x::text) FROM generate_series(0,20) x);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.fipshash(text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH cte1 AS MATERIALIZED (SELECT * FROM t1 WHERE f_leak(b)) SELECT * FROM cte1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) WITH cte1 AS MATERIALIZED (SELECT * FROM t1 WHERE f_leak(b)) SELECT * FROM cte1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"CTE Scan on cte1"#)],
                            &[T(r#"  CTE cte1"#)],
                            &[T(r#"    ->  Seq Scan on t1"#)],
                            &[T(r#"          Filter: (((a % 2) = 0) AND f_leak(b))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH cte1 AS (UPDATE t1 SET a = a + 1 RETURNING *) SELECT * FROM cte1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH cte1 AS (UPDATE t1 SET a = a RETURNING *) SELECT * FROM cte1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH cte1 AS (INSERT INTO t1 VALUES (21, 'Fail') RETURNING *) SELECT * FROM cte1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "t1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH cte1 AS (INSERT INTO t1 VALUES (20, 'Success') RETURNING *) SELECT * FROM cte1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("20"), T("Success")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY p1 ON t1 RENAME TO p1;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"policy "p1" for table "t1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT polname, relname FROM pg_policy pol JOIN pg_class pc ON (pc.oid = pol.polrelid) WHERE relname = 't1';",
                    expected: Expected::Rows {
                        columns: &[Column("polname", NAME), Column("relname", NAME)],
                        rows: &[
                            &[T("p1"), T("t1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY p1 ON t1 RENAME TO p2;",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT polname, relname FROM pg_policy pol JOIN pg_class pc ON (pc.oid = pol.polrelid) WHERE relname = 't1';",
                    expected: Expected::Rows {
                        columns: &[Column("polname", NAME), Column("relname", NAME)],
                        rows: &[
                            &[T("p2"), T("t1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (a integer, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (SELECT * FROM t1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO t2 (SELECT * FROM t1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Insert on t2"#)],
                            &[T(r#"  ->  Seq Scan on t1"#)],
                            &[T(r#"        Filter: ((a % 2) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("20"), T("Success")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on t2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 AS SELECT * FROM t1;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("20"), T("Success")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * INTO t4 FROM t1;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("20"), T("Success")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE blog (id integer, author text, post text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE comment (blog_id integer, message text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON blog, comment TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY blog_1 ON blog USING (id % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE blog ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO blog VALUES (1, 'alice', 'blog #1'), (2, 'bob', 'blog #1'), (3, 'alice', 'blog #2'), (4, 'alice', 'blog #3'), (5, 'john', 'blog #1');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO comment VALUES (1, 'cool blog'), (1, 'fun blog'), (3, 'crazy blog'), (5, 'what?'), (4, 'insane!'), (2, 'who did it?');",
                    expected: Expected::Tag("INSERT 0 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, author, message FROM blog JOIN comment ON id = blog_id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("author", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("4"), T("alice"), T("insane!")],
                            &[T("2"), T("bob"), T("who did it?")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, author, message FROM comment JOIN blog ON id = blog_id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("author", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("4"), T("alice"), T("insane!")],
                            &[T("2"), T("bob"), T("who did it?")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY comment_1 ON comment USING (blog_id < 4);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE comment ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, author, message FROM blog JOIN comment ON id = blog_id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("author", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("2"), T("bob"), T("who did it?")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, author, message FROM comment JOIN blog ON id = blog_id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("author", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("2"), T("bob"), T("who did it?")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE blog, comment;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p2 ON t1;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 OWNER TO regress_rls_alice;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("20"), T("Success")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on t1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("20"), T("Success")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on t1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE copy_t CASCADE;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"table "copy_t" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE copy_t (a integer, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON copy_t USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE copy_t ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON copy_t TO regress_rls_bob, regress_rls_exempt_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO copy_t (SELECT x, public.fipshash(x::text) FROM generate_series(0,10) x);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.fipshash(text) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 31, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE copy_rel_to (a integer, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON copy_rel_to USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE copy_rel_to ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON copy_rel_to TO regress_rls_bob, regress_rls_exempt_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO copy_rel_to VALUES (1, public.fipshash('1'));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.fipshash(unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 36, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE copy_rel_to_child () INHERITS (copy_rel_to);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO copy_rel_to_child VALUES (1, 'one'), (2, 'two');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE copy_t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE copy_rel_to CASCADE;",
                    expected: Expected::Tag("DROP TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to table copy_rel_to_child", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE current_check (currentid int, payload text, rlsuser text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON current_check TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO current_check VALUES (1, 'abc', 'regress_rls_bob'), (2, 'bcd', 'regress_rls_bob'), (3, 'cde', 'regress_rls_bob'), (4, 'def', 'regress_rls_bob');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON current_check FOR SELECT USING (currentid % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON current_check FOR DELETE USING (currentid = 4 AND rlsuser = current_user);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON current_check FOR UPDATE USING (currentid = 4) WITH CHECK (rlsuser = current_user);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE current_check ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM current_check;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("2"), T("bcd"), T("regress_rls_bob")],
                            &[T("4"), T("def"), T("regress_rls_bob")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE current_check SET payload = payload || '_new' WHERE currentid = 2 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                        ],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE current_check_cursor SCROLL CURSOR FOR SELECT * FROM current_check;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM current_check_cursor;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("2"), T("bcd"), T("regress_rls_bob")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE current_check SET payload = payload || '_new' WHERE CURRENT OF current_check_cursor RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                        ],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 1 FROM current_check_cursor;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("4"), T("def"), T("regress_rls_bob")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE current_check SET payload = payload || '_new' WHERE CURRENT OF current_check_cursor RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("4"), T("def_new"), T("regress_rls_bob")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM current_check;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("2"), T("bcd"), T("regress_rls_bob")],
                            &[T("4"), T("def_new"), T("regress_rls_bob")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE current_check SET payload = payload WHERE CURRENT OF current_check_cursor;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on current_check"#)],
                            &[T(r#"  ->  Tid Scan on current_check"#)],
                            &[T(r#"        TID Cond: CURRENT OF current_check_cursor"#)],
                            &[T(r#"        Filter: ((currentid = 4) AND ((currentid % 2) = 0))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM current_check_cursor;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("2"), T("bcd"), T("regress_rls_bob")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM current_check WHERE CURRENT OF current_check_cursor RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                        ],
                        tag: "DELETE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 1 FROM current_check_cursor;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("4"), T("def"), T("regress_rls_bob")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM current_check WHERE CURRENT OF current_check_cursor RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("4"), T("def_new"), T("regress_rls_bob")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM current_check;",
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("payload", TEXT), Column("rlsuser", TEXT)],
                        rows: &[
                            &[T("2"), T("bcd"), T("regress_rls_bob")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE current_check_2 (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO current_check_2 VALUES (1, 'Apple');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE current_check_2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE current_check_2 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON current_check_2 AS PERMISSIVE USING (ctid IN ('(0,1)', '(0,2)', '(4294967295,0)'));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, * FROM current_check_2;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("Apple")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE current_check_cursor CURSOR FOR SELECT * FROM current_check_2;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FROM current_check_cursor;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("Apple")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE current_check_2 SET b = 'Manzana' WHERE CURRENT OF current_check_cursor;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on current_check_2"#)],
                            &[T(r#"  ->  Tid Scan on current_check_2"#)],
                            &[T(r#"        TID Cond: CURRENT OF current_check_cursor"#)],
                            &[T(r#"        Filter: (ctid = ANY ('{"(0,1)","(0,2)","(4294967295,0)"}'::tid[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE current_check_2 SET b = 'Manzana' WHERE CURRENT OF current_check_cursor;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, * FROM current_check_2;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,2)"), T("1"), T("Manzana")],
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
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE current_check;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_security_active('current_check');",
                    expected: Expected::Rows {
                        columns: &[Column("row_security_active", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, most_common_vals FROM pg_stats WHERE tablename = 'current_check' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("most_common_vals", ANYARRAY)],
                        rows: &[
                            &[T("currentid"), Null],
                            &[T("payload"), Null],
                            &[T("rlsuser"), T("{regress_rls_bob}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_security_active('current_check');",
                    expected: Expected::Rows {
                        columns: &[Column("row_security_active", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, most_common_vals FROM pg_stats WHERE tablename = 'current_check' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("most_common_vals", ANYARRAY)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE coll_t (c) AS VALUES ('bar'::text);",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE POLICY coll_p ON coll_t USING (c < ('foo'::text COLLATE "C"));"#,
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE coll_t ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON coll_t TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (string_to_array(polqual, ':'))[7] AS inputcollid FROM pg_policy WHERE polrelid = 'coll_t'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("inputcollid", TEXT)],
                        rows: &[
                            &[T("inputcollid 950 ")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM coll_t;",
                    expected: Expected::Rows {
                        columns: &[Column("c", TEXT)],
                        rows: &[
                            &[T("bar")],
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
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_rls_eve;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_rls_frank;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tbl1 (c) AS VALUES ('bar'::text);",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON TABLE tbl1 TO regress_rls_eve;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY P ON tbl1 TO regress_rls_eve, regress_rls_frank USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refclassid::regclass, deptype FROM pg_depend WHERE classid = 'pg_policy'::regclass AND refobjid = 'tbl1'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("refclassid", REGCLASS), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_class"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refclassid::regclass, deptype FROM pg_shdepend WHERE classid = 'pg_policy'::regclass AND refobjid IN ('regress_rls_eve'::regrole, 'regress_rls_frank'::regrole);",
                    expected: Expected::Rows {
                        columns: &[Column("refclassid", REGCLASS), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_authid"), T("r")],
                            &[T("pg_authid"), T("r")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT q;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_rls_eve;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "regress_rls_eve" cannot be dropped because some objects depend on it"#, detail: "privileges for table tbl1\ntarget of policy p on table tbl1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO q;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY p ON tbl1 TO regress_rls_frank USING (true);",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT q;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_rls_eve;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "regress_rls_eve" cannot be dropped because some objects depend on it"#, detail: "privileges for table tbl1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO q;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALL ON TABLE tbl1 FROM regress_rls_eve;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT q;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_rls_eve;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO q;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT q;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_rls_frank;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "regress_rls_frank" cannot be dropped because some objects depend on it"#, detail: "target of policy p on table tbl1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO q;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p ON tbl1;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT q;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_rls_frank;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO q;",
                    expected: Expected::Tag("ROLLBACK"),
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
                    query: "CREATE TABLE t (c) AS VALUES ('bar'::text);",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p ON t USING (max(c));",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate functions are not allowed in policy expressions", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r2 (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r2 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON r1, r2 TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r1 USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r2 FOR SELECT USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON r2 FOR INSERT WITH CHECK (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON r2 FOR UPDATE USING (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p4 ON r2 FOR DELETE USING (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r2 VALUES (2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "r2""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r2 SET a = 2 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r2 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "DELETE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 SELECT a + 1 FROM r2 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("11")],
                            &[T("21")],
                        ],
                        tag: "INSERT 0 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r1 SET a = r2.a + 2 FROM r2 WHERE r1.a = r2.a RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("12"), T("10")],
                            &[T("22"), T("20")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r1 USING r2 WHERE r1.a = r2.a + 2 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("12"), T("10")],
                            &[T("22"), T("20")],
                        ],
                        tag: "DELETE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("11")],
                            &[T("21")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r1 USING (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r1 SET a = 1;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r1;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
                    query: "TABLE r1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "r1""#, hint: "To disable the policy for the table's owner, use ALTER TABLE NO FORCE ROW LEVEL SECURITY.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r1 SET a = 1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "r1""#, hint: "To disable the policy for the table's owner, use ALTER TABLE NO FORCE ROW LEVEL SECURITY.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "r1""#, hint: "To disable the policy for the table's owner, use ALTER TABLE NO FORCE ROW LEVEL SECURITY.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 (a int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r2 (a int REFERENCES r1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r2 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r2 USING (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r1;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"update or delete on table "r1" violates foreign key constraint "r2_a_fkey" on table "r2""#, detail: r#"Key (a)=(10) is still referenced from table "r2"."#, schema: "regress_rls_schema", table: "r2", constraint: "r2_a_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON r2;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 NO FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 DISABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r2;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r1 USING (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r2 VALUES (10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 (a int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r2 (a int REFERENCES r1 ON DELETE CASCADE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r2 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r2 USING (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r1;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 NO FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 (a int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r2 (a int REFERENCES r1 ON UPDATE CASCADE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r2 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r2 USING (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r1 SET a = a+5;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r2 NO FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("15")],
                            &[T("25")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r1 FOR SELECT USING (false);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON r1 FOR INSERT WITH CHECK (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10), (20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
                    query: "TABLE r1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "r1""#, hint: "To disable the policy for the table's owner, use ALTER TABLE NO FORCE ROW LEVEL SECURITY.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10), (20) RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 (a int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r1 FOR SELECT USING (a < 20);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON r1 FOR UPDATE USING (a < 20) WITH CHECK (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON r1 FOR INSERT WITH CHECK (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r1 SET a = 30;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 NO FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r1 SET a = 10;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TABLE r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r1 SET a = 30 RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10) ON CONFLICT (a) DO UPDATE SET a = 30 RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10) ON CONFLICT (a) DO UPDATE SET a = 30;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (10) ON CONFLICT ON CONSTRAINT r1_pkey DO UPDATE SET a = 30;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy for table "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 (a int, b int GENERATED ALWAYS AS (a * 10) VIRTUAL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 ADD c int GENERATED ALWAYS AS (a * 100) VIRTUAL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (1), (2), (4);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p0 ON r1 USING (b * 10 = c);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON r1 AS RESTRICTIVE USING (b > 10);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON r1 AS RESTRICTIVE USING ((SELECT c) < 400);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r1 FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy "p1" for table "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (4);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"new row violates row-level security policy "p2" for table "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r1 VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dep1 (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dep2 (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY dep_p1 ON dep1 TO regress_rls_bob USING (c1 > (select max(dep2.c1) from dep2));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER POLICY dep_p1 ON dep1 TO regress_rls_bob,regress_rls_carol;",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) = 1 FROM pg_depend WHERE objid = (SELECT oid FROM pg_policy WHERE polname = 'dep_p1') AND refobjid = (SELECT oid FROM pg_class WHERE relname = 'dep2');",
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
                    query: "ALTER POLICY dep_p1 ON dep1 USING (true);",
                    expected: Expected::Tag("ALTER POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) = 1 FROM pg_shdepend WHERE objid = (SELECT oid FROM pg_policy WHERE polname = 'dep_p1') AND refobjid = (SELECT oid FROM pg_authid WHERE rolname = 'regress_rls_bob');",
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
                    query: "SELECT count(*) = 1 FROM pg_shdepend WHERE objid = (SELECT oid FROM pg_policy WHERE polname = 'dep_p1') AND refobjid = (SELECT oid FROM pg_authid WHERE rolname = 'regress_rls_carol');",
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
                    query: "SELECT count(*) = 0 FROM pg_depend WHERE objid = (SELECT oid FROM pg_policy WHERE polname = 'dep_p1') AND refobjid = (SELECT oid FROM pg_class WHERE relname = 'dep2');",
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
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_rls_dob_role1;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_rls_dob_role2;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dob_t1 (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dob_t2 (c1 int) PARTITION BY RANGE (c1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON dob_t1 TO regress_rls_dob_role1 USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_rls_dob_role1;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON dob_t1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"policy "p1" for table "dob_t1" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON dob_t1 TO regress_rls_dob_role1,regress_rls_dob_role2 USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_rls_dob_role1;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON dob_t1;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON dob_t1 TO regress_rls_dob_role1,regress_rls_dob_role1 USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_rls_dob_role1;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON dob_t1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"policy "p1" for table "dob_t1" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON dob_t1 TO regress_rls_dob_role1,regress_rls_dob_role1,regress_rls_dob_role2 USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_rls_dob_role1;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON dob_t1;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON dob_t2 TO regress_rls_dob_role1,regress_rls_dob_role2 USING (true);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY regress_rls_dob_role1;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP POLICY p1 ON dob_t2;",
                    expected: Expected::Tag("DROP POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER regress_rls_dob_role1;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER regress_rls_dob_role2;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ref_tbl (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ref_tbl VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_tbl (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rls_tbl VALUES (10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_tbl ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON rls_tbl USING (EXISTS (SELECT 1 FROM ref_tbl));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ref_tbl TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_tbl TO regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rls_view AS SELECT * FROM rls_tbl;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER VIEW rls_view OWNER TO regress_rls_bob;",
                    expected: Expected::Tag("ALTER VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_view TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ref_tbl;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table ref_tbl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_tbl;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table rls_tbl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_view;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rls_tbl;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ref_tbl;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_tbl (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rls_tbl SELECT x/10 FROM generate_series(1, 100) x;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE rls_tbl;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_tbl ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_tbl TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION op_leak(int, int) RETURNS bool AS 'BEGIN RAISE NOTICE ''op_leak => %, %'', $1, $2; RETURN $1 < $2; END' LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <<< (procedure = op_leak, leftarg = int, rightarg = int, restrict = scalarltsel);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_tbl WHERE a <<< 1000;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rls_tbl WHERE a <<< 1000 or a <<< 900;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_child_tbl () INHERITS (rls_tbl);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rls_child_tbl SELECT x/10 FROM generate_series(1, 100) x;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE rls_child_tbl;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_ptbl (a int) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_part PARTITION OF rls_ptbl FOR VALUES FROM (-100) TO (100);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rls_ptbl SELECT x/10 FROM generate_series(1, 100) x;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE rls_ptbl, rls_part;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_ptbl ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_part ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_ptbl TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_part TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON rls_tbl USING (a < 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON rls_ptbl USING (a < 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON rls_part USING (a < 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_tbl WHERE a <<< 1000;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_child_tbl WHERE a <<< 1000;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table rls_child_tbl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_ptbl WHERE a <<< 1000;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_part WHERE a <<< 1000;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT * FROM rls_tbl UNION ALL SELECT * FROM rls_tbl) t WHERE a <<< 1000;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT * FROM rls_child_tbl UNION ALL SELECT * FROM rls_child_tbl) t WHERE a <<< 1000;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table rls_child_tbl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON rls_tbl FROM regress_rls_alice;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rls_tbl_view AS SELECT * FROM rls_tbl;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_child_tbl ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_child_tbl TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p4 ON rls_child_tbl USING (a < 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_tbl WHERE a <<< 1000;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table rls_tbl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_tbl_view WHERE a <<< 1000;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for view rls_tbl_view", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_child_tbl WHERE a <<< 1000;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT * FROM rls_tbl UNION ALL SELECT * FROM rls_tbl) t WHERE a <<< 1000;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table rls_tbl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT * FROM rls_child_tbl UNION ALL SELECT * FROM rls_child_tbl) t WHERE a <<< 1000;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR <<< (int, int);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION op_leak(int, int);",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rls_part;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rls_ptbl;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rls_child_tbl;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_tbl_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rls_tbl;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_tbl (a int, b int, c int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON rls_tbl USING (rls_tbl >= ROW(1,1,1));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_tbl ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_tbl FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rls_tbl SELECT 10, 20, 30;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) INSERT INTO rls_tbl SELECT * FROM (SELECT b, c FROM rls_tbl ORDER BY a) ss;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Insert on regress_rls_schema.rls_tbl"#)],
                            &[T(r#"  ->  Subquery Scan on ss"#)],
                            &[T(r#"        Output: ss.b, ss.c, NULL::integer"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Output: rls_tbl_1.b, rls_tbl_1.c, rls_tbl_1.a"#)],
                            &[T(r#"              Sort Key: rls_tbl_1.a"#)],
                            &[T(r#"              ->  Seq Scan on regress_rls_schema.rls_tbl rls_tbl_1"#)],
                            &[T(r#"                    Output: rls_tbl_1.b, rls_tbl_1.c, rls_tbl_1.a"#)],
                            &[T(r#"                    Filter: (rls_tbl_1.* >= '(1,1,1)'::record)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rls_tbl SELECT * FROM (SELECT b, c FROM rls_tbl ORDER BY a) ss;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rls_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("10"), T("20"), T("30")],
                            &[T("20"), T("30"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rls_tbl;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_t (c text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rls_t VALUES ('invisible to bob');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_t ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_t TO regress_rls_alice, regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON rls_t FOR SELECT TO regress_rls_alice USING (TRUE);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON rls_t FOR SELECT TO regress_rls_bob USING (FALSE);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION rls_f () RETURNS SETOF rls_t STABLE LANGUAGE sql AS $$ SELECT * FROM rls_t $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE q AS SELECT current_user, * FROM rls_f();",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
                        rows: &[
                            &[T("regress_rls_alice"), T("invisible to bob")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
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
                    query: "CREATE OR REPLACE FUNCTION rls_f() RETURNS SETOF rls_t STABLE LANGUAGE sql AS $$ WITH cte AS (SELECT * FROM rls_t) SELECT * FROM cte $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE r AS SELECT current_user, * FROM rls_f();",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE r;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
                        rows: &[
                            &[T("regress_rls_alice"), T("invisible to bob")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE r;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
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
                    query: "CREATE OR REPLACE FUNCTION rls_f() RETURNS SETOF rls_t STABLE LANGUAGE sql AS $$ SELECT * FROM (SELECT * FROM rls_t) _ $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE s AS SELECT current_user, * FROM rls_f();",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE s;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
                        rows: &[
                            &[T("regress_rls_alice"), T("invisible to bob")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE s;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
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
                    query: "CREATE OR REPLACE FUNCTION rls_f() RETURNS SETOF rls_t STABLE LANGUAGE sql AS $$ SELECT EXISTS(SELECT * FROM rls_t)::text $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE t AS SELECT current_user, * FROM rls_f();",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE t;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
                        rows: &[
                            &[T("regress_rls_alice"), T("true")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE t;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
                        rows: &[
                            &[T("regress_rls_bob"), T("false")],
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
                    query: "CREATE OR REPLACE FUNCTION rls_f() RETURNS SETOF rls_t STABLE LANGUAGE sql AS $$ SELECT * FROM (SELECT array_agg(c) AS cs FROM rls_t) _ GROUP BY cs $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE u AS SELECT current_user, * FROM rls_f();",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE u;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
                        rows: &[
                            &[T("regress_rls_alice"), T(r#"{"invisible to bob"}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE u;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
                        rows: &[
                            &[T("regress_rls_bob"), Null],
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
                    query: "CREATE VIEW rls_v WITH (security_invoker) AS SELECT * FROM rls_t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_v TO regress_rls_alice, regress_rls_bob;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION rls_f() RETURNS SETOF rls_t STABLE LANGUAGE sql AS $$ SELECT * FROM rls_v $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE v AS SELECT current_user, * FROM rls_f();",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE v;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
                        rows: &[
                            &[T("regress_rls_alice"), T("invisible to bob")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE v;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("c", TEXT)],
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
                    query: "DROP FUNCTION rls_f();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rls_v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rls_t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_t (c text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test_t (c text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rls_t VALUES ('a'), ('b'), ('c'), ('d');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test_t VALUES ('a'), ('b');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_t ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON rls_t TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON test_t TO regress_rls_alice;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON rls_t FOR SELECT TO regress_rls_alice USING (c = current_setting('rls_test.blah'));",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION rls_f(text) RETURNS text BEGIN ATOMIC SELECT set_config('rls_test.blah', $1, TRUE) || set_config('row_security', 'false', TRUE) || string_agg(c, ',' ORDER BY c) FROM rls_t; END;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plan_cache_mode TO force_custom_plan;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rls_f(c) FROM test_t ORDER BY rls_f;",
                    expected: Expected::Rows {
                        columns: &[Column("rls_f", TEXT)],
                        rows: &[
                            &[T("aoffa,b,c,d")],
                            &[T("boffa,b,c,d")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rls_f(c) FROM test_t ORDER BY rls_f;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "rls_t""#, context: Some(r#"SQL function "rls_f" statement 1"#), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plan_cache_mode TO force_generic_plan;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rls_f(c) FROM test_t ORDER BY rls_f;",
                    expected: Expected::Rows {
                        columns: &[Column("rls_f", TEXT)],
                        rows: &[
                            &[T("aoffa,b,c,d")],
                            &[T("boffa,b,c,d")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_rls_alice;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rls_f(c) FROM test_t ORDER BY rls_f;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "rls_t""#, context: Some(r#"SQL function "rls_f" statement 1"#), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET plan_cache_mode;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET rls_test.blah;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION rls_f(text);",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rls_t, test_t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA regress_rls_schema CASCADE;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 30 other objects", detail: "drop cascades to function f_leak(text)\ndrop cascades to table uaccount\ndrop cascades to table category\ndrop cascades to table document\ndrop cascades to table part_document\ndrop cascades to table dependent\ndrop cascades to table rec1\ndrop cascades to table rec2\ndrop cascades to view rec1v\ndrop cascades to view rec2v\ndrop cascades to table s1\ndrop cascades to table s2\ndrop cascades to view v2\ndrop cascades to table b1\ndrop cascades to view bv1\ndrop cascades to table z1\ndrop cascades to table z2\ndrop cascades to table z1_blacklist\ndrop cascades to table x1\ndrop cascades to table y1\ndrop cascades to table y2\ndrop cascades to table t1\ndrop cascades to table t2\ndrop cascades to table t3\ndrop cascades to table t4\ndrop cascades to table current_check\ndrop cascades to table dep1\ndrop cascades to table dep2\ndrop cascades to table dob_t1\ndrop cascades to table dob_t2", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER regress_rls_alice;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER regress_rls_bob;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER regress_rls_carol;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER regress_rls_dave;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER regress_rls_exempt_user;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_rls_group1;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_rls_group2;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA regress_rls_schema;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_tbl (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_tbl ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON rls_tbl USING (c1 > 5);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON rls_tbl FOR SELECT USING (c1 <= 3);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON rls_tbl FOR UPDATE USING (c1 <= 3) WITH CHECK (c1 > 5);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p4 ON rls_tbl FOR DELETE USING (c1 <= 3);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rls_tbl_force (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_tbl_force ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rls_tbl_force FORCE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON rls_tbl_force USING (c1 = 5) WITH CHECK (c1 < 5);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p2 ON rls_tbl_force FOR SELECT USING (c1 = 8);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p3 ON rls_tbl_force FOR UPDATE USING (c1 = 8) WITH CHECK (c1 >= 5);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p4 ON rls_tbl_force FOR DELETE USING (c1 = 8);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "COPY TO and COPY FROM under row-level security, from PG18's rowsecurity regression test",
            set_up_script: &[
                "CREATE ROLE regress_rls_bob NOLOGIN;",
                "CREATE ROLE regress_rls_carol NOLOGIN;",
                "CREATE ROLE regress_rls_exempt_user BYPASSRLS NOLOGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE copy_t (a integer, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON copy_t USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE copy_t ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON copy_t TO regress_rls_bob, regress_rls_exempt_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO copy_t (SELECT x, 'v' || x FROM generate_series(0,10) x);",
                    expected: Expected::Tag("INSERT 0 11"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM copy_t ORDER BY a ASC) TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 11"),
                    copy_to_stdout_file: "rls-copy-all.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM copy_t ORDER BY a ASC) TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 11"),
                    copy_to_stdout_file: "rls-copy-all.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM copy_t ORDER BY a ASC) TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "copy_t""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM copy_t ORDER BY a ASC) TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 6"),
                    copy_to_stdout_file: "rls-copy-even.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM copy_t ORDER BY a ASC) TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 11"),
                    copy_to_stdout_file: "rls-copy-all.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM copy_t ORDER BY a ASC) TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 11"),
                    copy_to_stdout_file: "rls-copy-all.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM copy_t ORDER BY a ASC) TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "copy_t""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM copy_t ORDER BY a ASC) TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table copy_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE copy_rel_to (a integer, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE POLICY p1 ON copy_rel_to USING (a % 2 = 0);",
                    expected: Expected::Tag("CREATE POLICY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE copy_rel_to ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON copy_rel_to TO regress_rls_bob, regress_rls_exempt_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO copy_rel_to VALUES (1, 'v1');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "rls-copy-one.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "rls-copy-one.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "copy_rel_to""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 0"),
                    copy_to_stdout_file: "copy-to-empty.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "rls-copy-one.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "rls-copy-one.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table copy_rel_to", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table copy_rel_to", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE copy_rel_to_child () INHERITS (copy_rel_to);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO copy_rel_to_child VALUES (1, 'one'), (2, 'two');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "rls-copy-one.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "rls-copy-one.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "copy_rel_to""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 0"),
                    copy_to_stdout_file: "copy-to-empty.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "rls-copy-one.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "rls-copy-one.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table copy_rel_to", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_rel_to TO STDOUT WITH DELIMITER ',';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table copy_rel_to", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_t FROM STDIN;",
                    expected: Expected::Tag("COPY 4"),
                    copy_from_stdin_file: "rls-copy-from.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_t FROM STDIN;",
                    expected: Expected::Tag("COPY 4"),
                    copy_from_stdin_file: "rls-copy-from.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_bob;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_t FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"query would be affected by row-level security policy for table "copy_t""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_t FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "COPY FROM not supported with row-level security", hint: "Use INSERT statements instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_exempt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_t FROM STDIN;",
                    expected: Expected::Tag("COPY 4"),
                    copy_from_stdin_file: "rls-copy-from.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION regress_rls_carol;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_t FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table copy_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET row_security TO ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copy_t FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table copy_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM copy_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[&[T("23")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE copy_t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE copy_rel_to CASCADE;",
                    expected: Expected::Tag("DROP TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to table copy_rel_to_child", ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_row_security_plans_after_role_changes() {
    run_scripts(&[
        ScriptTest {
            name: "a prepared query reads every row once its role may bypass row security",
            set_up_script: &[
                "CREATE ROLE rp_alice NOBYPASSRLS;",
                "CREATE TABLE rp (id INT, owner TEXT);",
                "INSERT INTO rp VALUES (1, 'rp_alice'), (2, 'rp_bob');",
                "ALTER TABLE rp ENABLE ROW LEVEL SECURITY;",
                "CREATE POLICY rp_own ON rp USING (owner = current_user);",
                "GRANT SELECT ON rp TO rp_alice;",
            ],
            assertions: &[
                ScriptTestAssertion { query: "SET ROLE rp_alice;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion { query: "PREPARE rp_q AS SELECT id FROM rp ORDER BY id;", expected: Expected::Tag("PREPARE"), ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "RESET ROLE;", expected: Expected::Tag("RESET"), ..A },
                ScriptTestAssertion { query: "ALTER ROLE rp_alice BYPASSRLS;", expected: Expected::Tag("ALTER ROLE"), ..A },
                ScriptTestAssertion { query: "SET ROLE rp_alice;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
                ScriptTestAssertion { query: "RESET ROLE;", expected: Expected::Tag("RESET"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a prepared query takes up a policy for a group once its role joins the group",
            set_up_script: &[
                "CREATE ROLE rp_alice;",
                "CREATE ROLE rp_group;",
                "CREATE TABLE rp (id INT, owner TEXT);",
                "INSERT INTO rp VALUES (1, 'rp_alice'), (2, 'rp_bob');",
                "ALTER TABLE rp ENABLE ROW LEVEL SECURITY;",
                "CREATE POLICY rp_own ON rp USING (owner = current_user);",
                "CREATE POLICY rp_all ON rp TO rp_group USING (true);",
                "GRANT SELECT ON rp TO rp_alice;",
            ],
            assertions: &[
                ScriptTestAssertion { query: "SET ROLE rp_alice;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion { query: "PREPARE rp_q AS SELECT id FROM rp ORDER BY id;", expected: Expected::Tag("PREPARE"), ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "RESET ROLE;", expected: Expected::Tag("RESET"), ..A },
                ScriptTestAssertion { query: "GRANT rp_group TO rp_alice;", expected: Expected::Tag("GRANT ROLE"), ..A },
                ScriptTestAssertion { query: "SET ROLE rp_alice;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
                ScriptTestAssertion { query: "RESET ROLE;", expected: Expected::Tag("RESET"), ..A },
                ScriptTestAssertion { query: "REVOKE rp_group FROM rp_alice;", expected: Expected::Tag("REVOKE ROLE"), ..A },
                ScriptTestAssertion { query: "SET ROLE rp_alice;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "RESET ROLE;", expected: Expected::Tag("RESET"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a prepared query takes up a policy for the database's owner once its role owns the database",
            set_up_script: &[
                "CREATE ROLE rp_alice CREATEDB;",
                "CREATE TABLE rp (id INT, owner TEXT);",
                "INSERT INTO rp VALUES (1, 'rp_alice'), (2, 'rp_bob');",
                "ALTER TABLE rp ENABLE ROW LEVEL SECURITY;",
                "CREATE POLICY rp_own ON rp USING (owner = current_user);",
                "CREATE POLICY rp_all ON rp TO pg_database_owner USING (true);",
                "GRANT SELECT ON rp TO rp_alice;",
            ],
            assertions: &[
                ScriptTestAssertion { query: "SET ROLE rp_alice;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion { query: "PREPARE rp_q AS SELECT id FROM rp ORDER BY id;", expected: Expected::Tag("PREPARE"), ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "RESET ROLE;", expected: Expected::Tag("RESET"), ..A },
                ScriptTestAssertion { query: "ALTER DATABASE postgres OWNER TO rp_alice;", expected: Expected::Tag("ALTER DATABASE"), ..A },
                ScriptTestAssertion { query: "SET ROLE rp_alice;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
                ScriptTestAssertion { query: "RESET ROLE;", expected: Expected::Tag("RESET"), ..A },
                ScriptTestAssertion { query: "ALTER DATABASE postgres OWNER TO postgres;", expected: Expected::Tag("ALTER DATABASE"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a prepared query reads every row once another session lets its role bypass row security",
            set_up_script: &[
                "CREATE ROLE rp_alice NOBYPASSRLS;",
                "CREATE TABLE rp (id INT, owner TEXT);",
                "INSERT INTO rp VALUES (1, 'rp_alice'), (2, 'rp_bob');",
                "ALTER TABLE rp ENABLE ROW LEVEL SECURITY;",
                "CREATE POLICY rp_own ON rp USING (owner = current_user);",
                "GRANT SELECT ON rp TO rp_alice;",
            ],
            assertions: &[
                ScriptTestAssertion { query: "SET ROLE rp_alice;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion { query: "PREPARE rp_q AS SELECT id FROM rp ORDER BY id;", expected: Expected::Tag("PREPARE"), ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "ALTER ROLE rp_alice BYPASSRLS;", expected: Expected::Tag("ALTER ROLE"), client: "other", ..A },
                ScriptTestAssertion {
                    query: "EXECUTE rp_q;",
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("1")], &[T("2")]], tag: "SELECT 2" },
                    ..A
                },
                ScriptTestAssertion { query: "RESET ROLE;", expected: Expected::Tag("RESET"), ..A },
            ],
            ..S
        },
    ]);
}
