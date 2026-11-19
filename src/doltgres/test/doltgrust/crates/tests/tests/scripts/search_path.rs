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
use harness::script::{A, Column, Diagnostic, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_search_path_inside_functions() {
    run_scripts(&[
        ScriptTest {
            name: "A function sees the search path its caller's settings, its own SET clause, a SET inside it, and a maintenance command's restricted path give",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE sp_t (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sp_t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp(i int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'path % rel % schemas %', current_setting('search_path'), 'public.sp_t'::regclass::text, current_schemas(true)::text; RETURN i; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sq(i int) RETURNS text IMMUTABLE LANGUAGE sql AS $$ SELECT current_schemas(true)::text || ' ' || 'public.sp_t'::regclass::text $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX sp_i ON sp_t (sp(a));",
                    expected: Expected::Tag("CREATE INDEX"),
                    notices: &[Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX sp_i;",
                    expected: Expected::Tag("REINDEX"),
                    notices: &[Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX sp_i;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW sp_m AS SELECT sq(a) FROM sp_t;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM sp_m;",
                    expected: Expected::Rows {
                        columns: &[Column("sq", TEXT)],
                        rows: &[
                            &[T("{pg_catalog} public.sp_t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sp_t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW sp_m;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM sp_m ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("sq", TEXT)],
                        rows: &[
                            &[T("{pg_catalog} public.sp_t")],
                            &[T("{pg_catalog} public.sp_t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS sp_s ON (sp(a)) FROM sp_t;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE sp_t;",
                    expected: Expected::Tag("ANALYZE"),
                    notices: &[Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }, Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }, Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }, Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (ANALYZE) sp_t;",
                    expected: Expected::Tag("VACUUM"),
                    notices: &[Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }, Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }, Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }, Diagnostic { code: "00000", message: "path pg_catalog, pg_temp rel public.sp_t schemas {pg_catalog}", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sc() RETURNS text LANGUAGE sql SET search_path = pg_catalog AS $$ SELECT current_schemas(true)::text || ' ' || 'public.sp_t'::regclass::text $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sc();",
                    expected: Expected::Rows {
                        columns: &[Column("sc", TEXT)],
                        rows: &[
                            &[T("{pg_catalog} public.sp_t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pc() RETURNS text LANGUAGE plpgsql SET search_path = pg_catalog AS $$ BEGIN RETURN current_schemas(true)::text || ' ' || 'public.sp_t'::regclass::text; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pc();",
                    expected: Expected::Rows {
                        columns: &[Column("pc", TEXT)],
                        rows: &[
                            &[T("{pg_catalog} public.sp_t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION dc() RETURNS text LANGUAGE plpgsql AS $$ BEGIN SET LOCAL search_path = pg_catalog; RETURN current_schemas(true)::text || ' ' || 'public.sp_t'::regclass::text; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dc();",
                    expected: Expected::Rows {
                        columns: &[Column("dc", TEXT)],
                        rows: &[
                            &[T("{pg_catalog} public.sp_t")],
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
                    query: "CREATE FUNCTION ds() RETURNS text LANGUAGE plpgsql AS $$ BEGIN SET LOCAL DateStyle = 'SQL, DMY'; RETURN current_setting('DateStyle') || ' ' || '2024-01-02'::date::text; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ds();",
                    expected: Expected::Rows {
                        columns: &[Column("ds", TEXT)],
                        rows: &[
                            &[T("SQL, DMY 02/01/2024")],
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
                    query: "CREATE SCHEMA shadow;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION shadow.lower(text) RETURNS text LANGUAGE sql AS $$ SELECT 'shadowed' $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION df(t text) RETURNS text LANGUAGE sql SECURITY DEFINER SET search_path = pg_catalog AS $$ SELECT lower(t) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION dp(t text) RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog AS $$ BEGIN RETURN lower(t); END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE sp_caller;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON SCHEMA shadow TO sp_caller;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sp_caller;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = shadow, pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lower('ABC') AS caller, public.df('ABC') AS sql_definer, public.dp('ABC') AS plpgsql_definer;",
                    expected: Expected::Rows {
                        columns: &[Column("caller", TEXT), Column("sql_definer", TEXT), Column("plpgsql_definer", TEXT)],
                        rows: &[
                            &[T("shadowed"), T("abc"), T("abc")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET search_path;",
                    expected: Expected::Tag("RESET"),
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
